/*
 * pmu.h — Per-vCPU Performance Monitor Unit manager (Phase 4A §3.2.1)
 *
 * The ARM PMU (Performance Monitor Unit) provides hardware event counters:
 *   PMCCNTR_EL0   — CPU cycle counter
 *   PMEVCNTR0-3   — programmable event counters (cycles, instructions, etc.)
 *   PMCR_EL0      — control register (E=enable, C=cycle reset, P=event reset)
 *   PMCNTENSET_EL0— counter enable set
 *   PMCNTENCLR_EL0— counter enable clear
 *   PMINTENSET_EL1— interrupt enable set
 *   PMOVSR_EL0    — overflow status register
 *
 * Hypervisor responsibility:
 *   1. Set MDCR_EL2.TPM=1, MDCR_EL2.TPMCR=1 so ALL guest PMU register
 *      accesses trap to EL2. This prevents one VM from reading another's
 *      hardware performance counters.
 *   2. On context switch: save current vCPU's PMU registers to pmu_ctx_t,
 *      restore next vCPU's PMU registers.
 *   3. Provide HVC_PMU_QUERY (0x0040) so guests can read their own counters
 *      without needing direct hardware access.
 *
 * Counter events tracked (QEMU cortex-a57 supports these):
 *   0x0008  INST_RETIRED     — instructions executed
 *   0x0011  CPU_CYCLES       — cycle counter (same as PMCCNTR)
 *   0x0004  L1D_CACHE_REFILL — L1 D-cache misses
 *   0x0003  L1D_CACHE        — L1 D-cache accesses
 */

#ifndef HYP_PMU_H
#define HYP_PMU_H

#include "../../include/types.h"
#include "../../include/error.h"

/* Number of programmable event counters to save/restore per vCPU */
#define PMU_NUM_COUNTERS  4

/*
 * pmu_ctx_t — saved PMU state for one vCPU.
 * Embedded inside vcpu_t; saved/restored on every context switch.
 */
typedef struct {
    u64 pmcr_el0;           /* PMU control register                  */
    u64 pmcntenset_el0;     /* counter enable bits                   */
    u64 pmccntr_el0;        /* CPU cycle counter                     */
    u64 pmccfiltr_el0;      /* cycle counter filter                  */
    u64 pmevcntr[PMU_NUM_COUNTERS];   /* event counter values        */
    u64 pmevtyper[PMU_NUM_COUNTERS];  /* event type/filter per counter*/
    u64 pmintenset_el1;     /* interrupt enable set                  */
    u64 pmovsr_el0;         /* overflow status register              */
    bool enabled;           /* true once guest has written PMCR.E=1 */
} pmu_ctx_t;

/*
 * pmu_init — called once at hypervisor boot.
 * Sets MDCR_EL2 to trap ALL guest PMU accesses to EL2.
 */
void pmu_init(void);

/*
 * pmu_vcpu_init — zero-initialise PMU context for a new vCPU.
 * Called from vcpu_init().
 */
void pmu_vcpu_init(pmu_ctx_t *ctx);

/*
 * pmu_save — save hardware PMU regs into ctx.
 * Called before switching away from a vCPU.
 */
void pmu_save(pmu_ctx_t *ctx);

/*
 * pmu_restore — restore PMU regs from ctx into hardware.
 * Called after switching to a vCPU.
 * If ctx->enabled is false, disables the PMU so the new vCPU
 * starts with counters off (preventing data leakage).
 */
void pmu_restore(const pmu_ctx_t *ctx);

/*
 * pmu_handle_trap — called from hyp_sync_handler when
 * ESR_EL2.EC == 0x18 (MSR/MRS trap from EL1/EL0 to PMU registers).
 *
 * Emulates the PMU register access against the current vCPU's pmu_ctx.
 * Returns E_OK if handled, E_UNSUPPORTED if the register is unknown.
 */
err_t pmu_handle_trap(void *regs, u64 esr);

/*
 * pmu_query — fill *cycles and *instrs with the calling vCPU's
 * accumulated counter values. Used by HVC_PMU_QUERY.
 */
err_t pmu_query(u64 *cycles, u64 *instrs, u64 *cache_misses);

/* MDCR_EL2 bit definitions */
#define MDCR_EL2_TPM    (1ULL << 6)   /* Trap PMU accesses from EL1/EL0 */
#define MDCR_EL2_TPMCR  (1ULL << 5)   /* Trap PMCR_EL0 accesses          */
#define MDCR_EL2_HPME   (1ULL << 7)   /* Enable hyp PMU events            */
#define MDCR_EL2_HPMN   0x1FUL        /* Hyp PMU event counter count mask  */

/* PMU register encoding helpers (for trap decode) */
#define PMU_REG_PMCR_EL0       0x9C00  /* Op0=3 Op1=3 CRn=9 CRm=12 Op2=0 */
#define PMU_REG_PMCNTENSET_EL0 0x9C01
#define PMU_REG_PMCNTENCLR_EL0 0x9C02
#define PMU_REG_PMOVSR_EL0     0x9C03
#define PMU_REG_PMCCNTR_EL0    0x9C1F
#define PMU_REG_PMCCFILTR_EL0  0x9E7F
#define PMU_REG_PMINTENSET_EL1 0x9C09
#define PMU_REG_PMINTENCLR_EL1 0x9C0A

#endif /* HYP_PMU_H */
