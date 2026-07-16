/*
 * pmu.c — Per-vCPU PMU manager (Phase 4A §3.2.1)
 *
 * How it works:
 *
 * 1. pmu_init() sets MDCR_EL2.TPM=1 and MDCR_EL2.TPMCR=1.
 *    From this point, any guest access to a PMU register (via MRS/MSR)
 *    traps to EL2 as EC=0x18 (System register trap).
 *
 * 2. pmu_save/pmu_restore are called from vcpu_do_switch().
 *    This gives each VM its own private view of the PMU.
 *    VM1 (Linux) can count its own instructions without seeing VM2's.
 *
 * 3. pmu_handle_trap() is called from hyp_sync_handler when EC=0x18
 *    and the trapped register is a PMU register. It emulates the
 *    MRS/MSR against the current vCPU's pmu_ctx — the guest never
 *    touches real hardware directly.
 *
 * 4. HVC_PMU_QUERY (implemented in hvc_handler.c) calls pmu_query()
 *    to expose the vCPU's accumulated cycle and instruction counts
 *    without requiring the guest to access hardware registers.
 *
 * Register isolation guarantees:
 *   - PMCCNTR_EL0: guest reads return pmu_ctx.pmccntr_el0 (virtualised)
 *   - On save: hardware PMCCNTR is read and ADDED to the accumulated total
 *     in pmu_ctx, then the hardware counter is reset. This prevents overflow
 *     and gives a running total across all the vCPU's scheduled intervals.
 *   - On restore: hardware counter is reset to 0 for the new vCPU.
 *     The vCPU's own saved value is available via MRS trap emulation.
 */

#include "pmu.h"
#include "vcpu.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

/* ── ARM64 PMU sysreg access macros ── */

#define READ_PMU(reg)  ({u64 _v; asm volatile("mrs %0, " #reg : "=r"(_v)); _v;})
#define WRITE_PMU(reg, val) asm volatile("msr " #reg ", %0" :: "r"((u64)(val)))

/* Temporarily disable PMU trapping so EL2 can access hardware PMU regs directly.
 * Must be used in pmu_save/pmu_restore to prevent recursive EL2 traps. */
static inline void pmu_trap_disable(void)
{
    u64 mdcr;
    asm volatile("mrs %0, mdcr_el2" : "=r"(mdcr));
    mdcr &= ~(MDCR_EL2_TPM | MDCR_EL2_TPMCR);
    asm volatile("msr mdcr_el2, %0; isb" :: "r"(mdcr));
}

static inline void pmu_trap_enable(void)
{
    u64 mdcr;
    asm volatile("mrs %0, mdcr_el2" : "=r"(mdcr));
    mdcr |= MDCR_EL2_TPM | MDCR_EL2_TPMCR;
    asm volatile("msr mdcr_el2, %0; isb" :: "r"(mdcr));
}

/* ── ESR_EL2 system register trap decode ── */

/* ESR_EL2 when EC=0x18: bits[20:5] = system register encoding */
#define ESR_SYS_REG(esr)  (((esr) >> 5) & 0xFFFF)
#define ESR_SYS_DIR(esr)  ((esr) & (1ULL << 0))   /* 1=read(MRS) 0=write(MSR) */
#define ESR_SYS_RT(esr)   (((esr) >> 5) & 0x1F)   /* Rt field (destination reg)*/

/* Re-extract Rt correctly: bits[9:5] */
#define ESR_RT(esr)       (((esr) >> 5) & 0x1F)

/* System register CRm/Op encoding for common PMU regs */
#define SYS_PMCR_EL0       ((3ULL<<19)|(3ULL<<16)|(9ULL<<10)|(12ULL<<5)|(0ULL<<2))
#define SYS_PMCNTENSET_EL0 ((3ULL<<19)|(3ULL<<16)|(9ULL<<10)|(12ULL<<5)|(1ULL<<2))
#define SYS_PMCNTENCLR_EL0 ((3ULL<<19)|(3ULL<<16)|(9ULL<<10)|(12ULL<<5)|(2ULL<<2))
#define SYS_PMOVSR_EL0     ((3ULL<<19)|(3ULL<<16)|(9ULL<<10)|(12ULL<<5)|(3ULL<<2))
#define SYS_PMCCNTR_EL0    ((3ULL<<19)|(3ULL<<16)|(9ULL<<10)|(13ULL<<5)|(0ULL<<2))
#define SYS_PMINTENSET_EL1 ((3ULL<<19)|(0ULL<<16)|(9ULL<<10)|(14ULL<<5)|(1ULL<<2))
#define SYS_PMINTENCLR_EL1 ((3ULL<<19)|(0ULL<<16)|(9ULL<<10)|(14ULL<<5)|(2ULL<<2))

/* Simpler: use the ISS encoding directly — Op0:Op1:CRn:CRm:Op2 packed */
static inline u32 sys_reg_enc(u32 op0, u32 op1, u32 crn, u32 crm, u32 op2)
{
    return (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}

#define ENC_PMCR_EL0        sys_reg_enc(3,3,9,12,0)
#define ENC_PMCNTENSET_EL0  sys_reg_enc(3,3,9,12,1)
#define ENC_PMCNTENCLR_EL0  sys_reg_enc(3,3,9,12,2)
#define ENC_PMOVSR_EL0      sys_reg_enc(3,3,9,12,3)
#define ENC_PMSWINC_EL0     sys_reg_enc(3,3,9,12,4)
#define ENC_PMCCNTR_EL0     sys_reg_enc(3,3,9,13,0)
#define ENC_PMCCFILTR_EL0   sys_reg_enc(3,3,14,15,7)
#define ENC_PMINTENSET_EL1  sys_reg_enc(3,0,9,14,1)
#define ENC_PMINTENCLR_EL1  sys_reg_enc(3,0,9,14,2)

/* Encodings for event counters 0-3 (PMEVCNTRn_EL0, PMEVTYPERn_EL0) */
static u32 evcntr_enc[PMU_NUM_COUNTERS] = {
    0xDF40U,
    0xDF41U,
    0xDF42U,
    0xDF43U,
};
static u32 evtyper_enc[PMU_NUM_COUNTERS] = {
    0xDF60U,
    0xDF61U,
    0xDF62U,
    0xDF63U,
};

/* ── pmu_init ── */

void pmu_init(void)
{
    u64 mdcr;
    asm volatile("mrs %0, mdcr_el2" : "=r"(mdcr));

    /* Set TPM=1: trap PMU access from EL1/EL0 to EL2
     * Set TPMCR=1: also trap PMCR_EL0 specifically */
    mdcr |= MDCR_EL2_TPM | MDCR_EL2_TPMCR;

    asm volatile("msr mdcr_el2, %0; isb" :: "r"(mdcr));

    LOG_INFO("PMU: MDCR_EL2=%lx — guest PMU trapped to EL2", mdcr);
}

/* ── pmu_vcpu_init ── */

void pmu_vcpu_init(pmu_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    /* Default: cycle counter filter passes EL0+EL1 counts */
    ctx->pmccfiltr_el0 = 0ULL;
    ctx->enabled = false;
}

/* ── pmu_save ── */

void pmu_save(pmu_ctx_t *ctx)
{
    if (!ctx) return;

    /* Disable PMU trapping so we can read hardware regs without recursive trap */
    pmu_trap_disable();

    /* Read and accumulate the hardware cycle counter */
    u64 hw_cycles = READ_PMU(pmccntr_el0);
    ctx->pmccntr_el0 += hw_cycles;

    /* Save PMU control state */
    ctx->pmcr_el0       = READ_PMU(pmcr_el0);
    ctx->pmcntenset_el0 = READ_PMU(pmcntenset_el0);
    ctx->pmovsr_el0     = READ_PMU(pmovsclr_el0);

    /* Save event counters — QEMU supports up to 4 */
    /* We use inline asm with indexed registers */
    u64 v;
    asm volatile("mrs %0, pmevcntr0_el0" : "=r"(v)); ctx->pmevcntr[0] = v;
    asm volatile("mrs %0, pmevcntr1_el0" : "=r"(v)); ctx->pmevcntr[1] = v;
    asm volatile("mrs %0, pmevcntr2_el0" : "=r"(v)); ctx->pmevcntr[2] = v;
    asm volatile("mrs %0, pmevcntr3_el0" : "=r"(v)); ctx->pmevcntr[3] = v;

    asm volatile("mrs %0, pmevtyper0_el0" : "=r"(v)); ctx->pmevtyper[0] = v;
    asm volatile("mrs %0, pmevtyper1_el0" : "=r"(v)); ctx->pmevtyper[1] = v;
    asm volatile("mrs %0, pmevtyper2_el0" : "=r"(v)); ctx->pmevtyper[2] = v;
    asm volatile("mrs %0, pmevtyper3_el0" : "=r"(v)); ctx->pmevtyper[3] = v;

    /* Disable PMU hardware so next vCPU starts clean */
    u64 pmcr_off = ctx->pmcr_el0 & ~(1ULL); /* clear E bit */
    WRITE_PMU(pmcr_el0, pmcr_off);
    /* Reset cycle counter and event counters */
    WRITE_PMU(pmcr_el0, pmcr_off | (1ULL << 2) | (1ULL << 1)); /* P=1 C=1 */

    /* Re-enable trapping for guest */
    pmu_trap_enable();
}

/* ── pmu_restore ── */

void pmu_restore(const pmu_ctx_t *ctx)
{
    if (!ctx) return;

    /* Disable PMU trapping so we can write hardware regs without recursive trap */
    pmu_trap_disable();

    /* Restore event types first (defines what counters measure) */
    WRITE_PMU(pmevtyper0_el0, ctx->pmevtyper[0]);
    WRITE_PMU(pmevtyper1_el0, ctx->pmevtyper[1]);
    WRITE_PMU(pmevtyper2_el0, ctx->pmevtyper[2]);
    WRITE_PMU(pmevtyper3_el0, ctx->pmevtyper[3]);

    /* Restore event counter values */
    WRITE_PMU(pmevcntr0_el0, ctx->pmevcntr[0]);
    WRITE_PMU(pmevcntr1_el0, ctx->pmevcntr[1]);
    WRITE_PMU(pmevcntr2_el0, ctx->pmevcntr[2]);
    WRITE_PMU(pmevcntr3_el0, ctx->pmevcntr[3]);

    /* Cycle counter: reset hardware to 0 — the guest reads the
     * accumulated total via MRS trap emulation, not hardware directly */
    u64 pmcr_reset = (1ULL << 2) | (1ULL << 1); /* P=1 C=1: reset counters */
    WRITE_PMU(pmcr_el0, pmcr_reset);

    /* Restore overflow status and interrupt enables */
    WRITE_PMU(pmovsclr_el0, ctx->pmovsr_el0);

    /* Restore enable bits and control last */
    WRITE_PMU(pmcntenset_el0, ctx->pmcntenset_el0);

    if (ctx->enabled) {
        /* Re-enable PMU with guest's PMCR settings (E bit set) */
        WRITE_PMU(pmcr_el0, ctx->pmcr_el0 | 1ULL);
    }
    /* If !enabled: leave PMU disabled — guest hasn't set E=1 yet */

    /* Re-enable trapping for guest */
    pmu_trap_enable();
}

/* ── pmu_handle_trap ── */

/*
 * Called from hyp_sync_handler when EC=0x18 (MSR/MRS to system register).
 * We check if the register is a PMU register and emulate it.
 * regs = pointer to guest x[0..30] array (same as hvc_dispatch).
 */
err_t pmu_handle_trap(void *regs, u64 esr)
{
    u64 *x = (u64 *)regs;

    /* Extract ISS fields */
    u32 iss   = (u32)(esr & 0x1FFFFFF);
    u32 rt    = (iss >> 5) & 0x1F;          /* destination/source Rt */
    bool is_read = (iss & 1) != 0;          /* 1=MRS(read) 0=MSR(write) */

    /* System register encoding: Op0[21:20] Op1[19:17] CRn[16:12] CRm[11:8] Op2[7:5] */
    /* Repack into our encoding format: (op0<<14)|(op1<<11)|(crn<<7)|(crm<<3)|op2 */
    u32 op0 = (iss >> 20) & 0x3;
    u32 op1 = (iss >> 17) & 0x7;
    u32 crn = (iss >> 12) & 0xF;
    u32 crm = (iss >> 8)  & 0xF;
    u32 op2 = (iss >> 5)  & 0x7;
    u32 enc = sys_reg_enc(op0, op1, crn, crm, op2);

    /* Get current vCPU's PMU context.
     * If no vCPU is running yet (early boot), return 0 for reads and
     * ignore writes — do NOT return E_INVAL which causes a fault log storm. */
    extern vcpu_t *g_current_vcpu[];
    vcpu_t *vc = g_current_vcpu[0];
    if (!vc) {
        /* Return 0 for MRS, silently ignore MSR */
        if (is_read && rt < 31) {
            u64 *x = (u64 *)regs;
            x[rt] = 0ULL;
        }
        return E_OK;
    }
    pmu_ctx_t *ctx = &vc->pmu;

    u64 val = (rt < 31) ? x[rt] : 0ULL;  /* value to write (MSR) */

    if (enc == ENC_PMCR_EL0) {
        if (is_read) {
            if (rt < 31) x[rt] = ctx->pmcr_el0;
        } else {
            ctx->pmcr_el0 = val;
            if (val & 1ULL) ctx->enabled = true;
            /* If guest resets counters (P=1 or C=1), reset accumulated totals */
            if (val & (1ULL << 1)) ctx->pmccntr_el0 = 0;
            if (val & (1ULL << 2)) {
                for (u32 i = 0; i < PMU_NUM_COUNTERS; i++)
                    ctx->pmevcntr[i] = 0;
            }
        }
        return E_OK;
    }

    if (enc == ENC_PMCNTENSET_EL0) {
        if (is_read) { if (rt < 31) x[rt] = ctx->pmcntenset_el0; }
        else         { ctx->pmcntenset_el0 |= val; }
        return E_OK;
    }

    if (enc == ENC_PMCNTENCLR_EL0) {
        if (is_read) { if (rt < 31) x[rt] = ctx->pmcntenset_el0; }
        else         { ctx->pmcntenset_el0 &= ~val; }
        return E_OK;
    }

    if (enc == ENC_PMOVSR_EL0) {
        if (is_read) { if (rt < 31) x[rt] = ctx->pmovsr_el0; }
        else         { ctx->pmovsr_el0 &= ~val; } /* write-1-to-clear */
        return E_OK;
    }

    if (enc == ENC_PMCCNTR_EL0) {
        if (is_read) {
            /* Return accumulated total — includes all past scheduled intervals */
            if (rt < 31) x[rt] = ctx->pmccntr_el0;
        } else {
            ctx->pmccntr_el0 = val;
        }
        return E_OK;
    }

    if (enc == ENC_PMCCFILTR_EL0) {
        if (is_read) { if (rt < 31) x[rt] = ctx->pmccfiltr_el0; }
        else         { ctx->pmccfiltr_el0 = val; }
        return E_OK;
    }

    if (enc == ENC_PMINTENSET_EL1) {
        if (is_read) { if (rt < 31) x[rt] = ctx->pmintenset_el1; }
        else         { ctx->pmintenset_el1 |= val; }
        return E_OK;
    }

    if (enc == ENC_PMINTENCLR_EL1) {
        if (is_read) { if (rt < 31) x[rt] = ctx->pmintenset_el1; }
        else         { ctx->pmintenset_el1 &= ~val; }
        return E_OK;
    }

    /* Event counters 0-3 */
    for (u32 i = 0; i < PMU_NUM_COUNTERS; i++) {
        if (enc == evcntr_enc[i]) {
            if (is_read) { if (rt < 31) x[rt] = ctx->pmevcntr[i]; }
            else         { ctx->pmevcntr[i] = val; }
            return E_OK;
        }
        if (enc == evtyper_enc[i]) {
            if (is_read) { if (rt < 31) x[rt] = ctx->pmevtyper[i]; }
            else         { ctx->pmevtyper[i] = val; }
            return E_OK;
        }
    }

    /* Unknown PMU register — return 0 for reads, ignore writes */
    if (is_read && rt < 31) x[rt] = 0ULL;
    return E_OK;  /* not E_UNSUPPORTED — unknown regs must not fault */
}

/* ── pmu_query ── */

err_t pmu_query(u64 *cycles, u64 *instrs, u64 *cache_misses)
{
    pmu_trap_disable();  /* prevent recursive trap when reading hardware */
    extern vcpu_t *g_current_vcpu[];
    vcpu_t *vc = g_current_vcpu[0];
    if (!vc) {
        if (cycles)       *cycles       = 0;
        if (instrs)       *instrs       = 0;
        if (cache_misses) *cache_misses = 0;
        return E_OK;
    }

    pmu_ctx_t *ctx = &vc->pmu;

    /* Accumulated cycle count (includes all past intervals + current hardware) */
    u64 hw_now = READ_PMU(pmccntr_el0);
    if (cycles)      *cycles      = ctx->pmccntr_el0 + hw_now;

    /* Event counters — report based on configured event types */
    if (instrs)      *instrs      = 0;
    if (cache_misses)*cache_misses = 0;

    for (u32 i = 0; i < PMU_NUM_COUNTERS; i++) {
        u32 event = (u32)(ctx->pmevtyper[i] & 0xFFFF);
        if (event == 0x0008 && instrs)       *instrs      += ctx->pmevcntr[i];
        if (event == 0x0003 && cache_misses) *cache_misses += ctx->pmevcntr[i];
    }

    pmu_trap_enable();
    return E_OK;
}
