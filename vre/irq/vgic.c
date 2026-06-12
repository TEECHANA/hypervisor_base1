/*
 * vgic.c — Virtual GIC distributor/CPU interface (Phase 4B §2.3.3)
 *
 * This file implements the per-VM virtual GIC state and the emulation
 * of GICD MMIO accesses and ICC system register accesses from guests.
 *
 * Key design decisions:
 *
 * Priority storage:
 *   GICD_IPRIORITYR stores one byte per IRQ at offset 0x400+irq.
 *   We mirror this 1:1 in vgic_dist_t.priority[256].
 *   Default priority 0xA0 (160) is used for all IRQs at reset,
 *   matching GICv3 reset state and our previous hardcoded value.
 *
 * Enable bitmap:
 *   GICD_ISENABLER/ICENABLER are bit arrays, one bit per IRQ.
 *   We store as u32 enable[8] (256 bits total).
 *   At reset: SGIs (0-15) are enabled, all SPIs (32+) disabled.
 *   Linux will write ISENABLER to enable the IRQs it needs.
 *
 * Priority mask (ICC_PMR_EL1):
 *   Lower numerical value = higher priority in ARM GIC.
 *   icc_pmr=0xFF means "allow all". icc_pmr=0x00 means "block all".
 *   An IRQ is delivered only if its priority < icc_pmr.
 *   Default: 0xFF (pass all) so boot works before Linux sets the mask.
 *
 * Integration with gic_inject_virq_lr():
 *   The updated gic_inject_virq_lr() (in gicv3.c) calls:
 *     vgic_get_priority(dist, virq)  → uses for ICH_LR priority field
 *     vgic_is_enabled(dist, virq)    → skips injection if disabled
 *     vgic_is_masked(cpu, priority)  → skips injection if PMR masked
 */

#include "vgic.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../core/vcpu/vcpu.h"

/* ── vgic_dist_init ── */

void vgic_dist_init(vgic_dist_t *dist)
{
    if (!dist) return;
    memset(dist, 0, sizeof(*dist));

    /* GICv3 reset: all IRQs at default priority 0xA0 */
    for (u32 i = 0; i < VGIC_MAX_IRQS; i++)
        dist->priority[i] = 0xA0;

    /* SGIs (0-15) enabled at reset per GICv3 spec */
    dist->enable[0] = 0x0000FFFFu;

    dist->initialised = true;
}

/* ── vgic_cpu_init ── */

void vgic_cpu_init(vgic_cpu_t *cpu)
{
    if (!cpu) return;
    /* PMR=0xFF: allow all IRQs through until guest configures it */
    cpu->icc_pmr    = 0xFF;
    cpu->icc_bpr0   = 0x03;  /* GICv3 reset value */
    cpu->icc_grpen1 = 0x01;  /* group 1 enabled by default */
}

/* ── Bitmap helpers ── */

static inline bool bitmap_get(const u32 *bm, u32 bit)
{
    return (bm[bit >> 5] >> (bit & 31)) & 1u;
}

static inline void bitmap_set(u32 *bm, u32 bit)
{
    bm[bit >> 5] |= (1u << (bit & 31));
}

static inline void bitmap_clr(u32 *bm, u32 bit)
{
    bm[bit >> 5] &= ~(1u << (bit & 31));
}

/* ── vgic_dist_write ── */

err_t vgic_dist_write(vgic_dist_t *dist, u32 offset, u32 val)
{
    if (!dist || !dist->initialised) return E_INVAL;

    /* GICD_ISENABLER: set enable bits (write-1-to-set) */
    if (offset >= GICD_OFF_ISENABLER_BASE && offset <= GICD_OFF_ISENABLER_END) {
        u32 word = (offset - GICD_OFF_ISENABLER_BASE) / 4;
        if (word < VGIC_NUM_WORDS) {
            dist->enable[word] |= val;
        }
        return E_OK;
    }

    /* GICD_ICENABLER: clear enable bits (write-1-to-clear) */
    if (offset >= GICD_OFF_ICENABLER_BASE && offset <= GICD_OFF_ICENABLER_END) {
        u32 word = (offset - GICD_OFF_ICENABLER_BASE) / 4;
        if (word < VGIC_NUM_WORDS) {
            dist->enable[word] &= ~val;
            /* SGIs (0-15) cannot be disabled per spec */
            dist->enable[0] |= 0x0000FFFFu;
        }
        return E_OK;
    }

    /* GICD_IPRIORITYR: store priority bytes */
    if (offset >= GICD_OFF_IPRIORITY_BASE && offset <= GICD_OFF_IPRIORITY_END) {
        u32 base_irq = (offset - GICD_OFF_IPRIORITY_BASE);
        /* 4 priority bytes packed per 32-bit register */
        for (u32 b = 0; b < 4; b++) {
            u32 irq = base_irq + b;
            if (irq < VGIC_MAX_IRQS) {
                dist->priority[irq] = (u8)((val >> (b * 8)) & 0xFF);
            }
        }
        return E_OK;
    }

    /* Other GICD writes — silently ignore (CTLR, ITARGETSR, etc.) */
    return E_OK;
}

/* ── vgic_dist_read ── */

err_t vgic_dist_read(vgic_dist_t *dist, u32 offset, u32 *val)
{
    if (!dist || !val) return E_INVAL;

    *val = 0;

    if (!dist->initialised) return E_OK;

    /* GICD_ISENABLER read */
    if (offset >= GICD_OFF_ISENABLER_BASE && offset <= GICD_OFF_ISENABLER_END) {
        u32 word = (offset - GICD_OFF_ISENABLER_BASE) / 4;
        if (word < VGIC_NUM_WORDS) *val = dist->enable[word];
        return E_OK;
    }

    /* GICD_ICENABLER read — returns same enable state */
    if (offset >= GICD_OFF_ICENABLER_BASE && offset <= GICD_OFF_ICENABLER_END) {
        u32 word = (offset - GICD_OFF_ICENABLER_BASE) / 4;
        if (word < VGIC_NUM_WORDS) *val = dist->enable[word];
        return E_OK;
    }

    /* GICD_IPRIORITYR read */
    if (offset >= GICD_OFF_IPRIORITY_BASE && offset <= GICD_OFF_IPRIORITY_END) {
        u32 base_irq = (offset - GICD_OFF_IPRIORITY_BASE);
        for (u32 b = 0; b < 4; b++) {
            u32 irq = base_irq + b;
            if (irq < VGIC_MAX_IRQS)
                *val |= ((u32)dist->priority[irq]) << (b * 8);
        }
        return E_OK;
    }

    return E_OK;
}

/* ── vgic_icc_trap ── */

/*
 * Called from the EC=0x18 handler in boot/main.c when the trapped
 * system register is an ICC register (not a PMU register).
 * pmu_handle_trap() is called first; if it returns E_UNSUPPORTED,
 * we try here. But since pmu_handle_trap returns E_OK for unknowns,
 * we instead decode ICC regs before PMU in the dispatcher.
 *
 * Encoding: same ISS format as PMU — Op0:Op1:CRn:CRm:Op2
 */
err_t vgic_icc_trap(vgic_cpu_t *cpu, void *regs, u64 esr)
{
    if (!cpu || !regs) return E_INVAL;

    u64 *x = (u64 *)regs;
    u32 iss      = (u32)(esr & 0x1FFFFFF);
    u32 rt       = (iss >> 5) & 0x1F;
    bool is_read = (iss & 1) != 0;

    u32 op0 = (iss >> 20) & 0x3;
    u32 op1 = (iss >> 17) & 0x7;
    u32 crn = (iss >> 12) & 0xF;
    u32 crm = (iss >> 8)  & 0xF;
    u32 op2 = (iss >> 5)  & 0x7;

    /* Pack into our encoding: (op0<<14)|(op1<<11)|(crn<<7)|(crm<<3)|op2 */
    u32 enc = (op0<<14)|(op1<<11)|(crn<<7)|(crm<<3)|op2;

    u64 val = (rt < 31) ? x[rt] : 0ULL;

    /* ICC_PMR_EL1: Op0=3 Op1=0 CRn=4 CRm=6 Op2=0 */
    if (enc == ICC_PMR_EL1_ENC) {
        if (is_read) {
            if (rt < 31) x[rt] = cpu->icc_pmr;
        } else {
            cpu->icc_pmr = (u8)(val & 0xFF);
        }
        return E_OK;
    }

    /* ICC_BPR0_EL1: Op0=3 Op1=0 CRn=12 CRm=8 Op2=3 */
    if (enc == ICC_BPR0_EL1_ENC) {
        if (is_read) {
            if (rt < 31) x[rt] = cpu->icc_bpr0;
        } else {
            cpu->icc_bpr0 = (u8)(val & 0x7);
        }
        return E_OK;
    }

    /* ICC_GRPEN1_EL1: Op0=3 Op1=0 CRn=12 CRm=12 Op2=7 */
    if (enc == ICC_GRPEN1_EL1_ENC) {
        if (is_read) {
            if (rt < 31) x[rt] = cpu->icc_grpen1;
        } else {
            cpu->icc_grpen1 = (u32)(val & 1);
        }
        return E_OK;
    }

    /* Not an ICC register we handle */
    return E_UNSUPPORTED;
}

/* ── vgic_get_priority ── */

u8 vgic_get_priority(const vgic_dist_t *dist, u32 irq)
{
    if (!dist || !dist->initialised || irq >= VGIC_MAX_IRQS)
        return 0xA0;  /* default */
    return dist->priority[irq];
}

/* ── vgic_is_enabled ── */

bool vgic_is_enabled(const vgic_dist_t *dist, u32 irq)
{
    if (!dist || !dist->initialised || irq >= VGIC_MAX_IRQS)
        return true;  /* default: allow injection if no state */
    return bitmap_get(dist->enable, irq);
}

/* ── vgic_is_masked ── */

bool vgic_is_masked(const vgic_cpu_t *cpu, u8 irq_priority)
{
    if (!cpu) return false;
    /* In ARM GIC: IRQ delivered if priority < PMR (lower = higher priority)
     * PMR=0xFF → pass all. PMR=0x00 → block all. */
    return irq_priority >= cpu->icc_pmr;
}
