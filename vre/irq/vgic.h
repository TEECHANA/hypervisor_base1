/*
 * vgic.h — Virtual GIC distributor/CPU interface state (Phase 4B §2.3.3)
 *
 * Problem this solves:
 *   Currently gic_inject_virq_lr() always uses priority 0xA0 (hardcoded).
 *   Linux programs GICD_IPRIORITYR registers expecting its chosen priorities
 *   to be honoured. When two vIRQs are both pending in LRs with the same
 *   priority, the GIC hardware picks arbitrarily. Real systems need the
 *   guest-programmed priority order to be preserved.
 *
 *   Also: Linux calls `msr icc_pmr_el1, x0` to set its IRQ priority mask.
 *   Without trapping this, ALL IRQs at any priority reach the guest,
 *   which is incorrect — a guest that masks low-priority interrupts should
 *   not receive them.
 *
 * What Phase 4B implements:
 *   1. vgic_dist_t — per-VM virtual distributor state:
 *      - 256-entry priority table  (mirrors GICD_IPRIORITYR)
 *      - 256-bit enable bitmap     (mirrors GICD_ISENABLER)
 *      - 256-bit pending bitmap    (set by irq_router on injection)
 *
 *   2. vgic_cpu_t — per-vCPU virtual CPU interface state:
 *      - icc_pmr  : priority mask (from `msr icc_pmr_el1`)
 *      - icc_bpr0 : binary point register
 *      - icc_grpen1: group 1 enable
 *
 *   3. MMIO trap extension in gic_dist_emulate():
 *      GICD_IPRIORITYR writes → stored in vgic_dist_t.priority[]
 *      GICD_ISENABLER  writes → set bits in vgic_dist_t.enable[]
 *      GICD_ICENABLER  writes → clear bits in vgic_dist_t.enable[]
 *
 *   4. ICC system register trap (EC=0x18, same path as PMU):
 *      icc_pmr_el1  → stored in vgic_cpu_t.icc_pmr
 *      icc_grpen1_el1 → stored in vgic_cpu_t.icc_grpen1
 *
 *   5. gic_inject_virq_lr() updated: reads priority from vgic_dist_t
 *      for the injected vIRQ, uses it in ICH_LR instead of hardcoded 0xA0.
 *      Also checks enable[] — disabled IRQs are not injected.
 *
 * Hardware register layout (ARM GICv3):
 *   GICD_IPRIORITYR: one byte per IRQ, address = 0x400 + irq
 *   GICD_ISENABLER:  one bit  per IRQ, address = 0x100 + (irq/32)*4
 *   GICD_ICENABLER:  one bit  per IRQ, address = 0x180 + (irq/32)*4
 *   ICC_PMR_EL1:     8-bit priority mask, EL1 system register
 */

#ifndef HYP_VGIC_H
#define HYP_VGIC_H

#include "../../include/types.h"
#include "../../include/error.h"

#define VGIC_MAX_IRQS   256
#define VGIC_NUM_WORDS  (VGIC_MAX_IRQS / 32)   /* 8 u32 words for bitmaps */

/*
 * vgic_dist_t — per-VM virtual GIC distributor state.
 * One instance per VM, stored in vm_t.
 */
typedef struct {
    u8  priority[VGIC_MAX_IRQS];    /* guest-programmed IRQ priorities     */
    u32 enable[VGIC_NUM_WORDS];     /* IRQ enable bits (1=enabled)         */
    u32 pending[VGIC_NUM_WORDS];    /* software-pending bits               */
    bool initialised;
} vgic_dist_t;

/*
 * vgic_cpu_t — per-vCPU virtual CPU interface state.
 * One instance per vcpu_t.
 */
typedef struct {
    u8  icc_pmr;        /* priority mask register — IRQs >= this are masked */
    u8  icc_bpr0;       /* binary point register (preemption grouping)       */
    u32 icc_grpen1;     /* group 1 interrupt enable                          */
} vgic_cpu_t;

/* ── API ── */

/* Initialise a VM's virtual distributor with GICv3 reset defaults */
void vgic_dist_init(vgic_dist_t *dist);

/* Initialise a vCPU's virtual CPU interface */
void vgic_cpu_init(vgic_cpu_t *cpu);

/* Handle a GICD MMIO write from a guest (called from gic_dist_emulate) */
err_t vgic_dist_write(vgic_dist_t *dist, u32 offset, u32 val);

/* Handle a GICD MMIO read from a guest */
err_t vgic_dist_read(vgic_dist_t *dist, u32 offset, u32 *val);

/* Handle ICC system register trap (EC=0x18, called from vgic_sysreg_trap) */
err_t vgic_icc_trap(vgic_cpu_t *cpu, void *regs, u64 esr);

/*
 * vgic_get_priority — return the guest-programmed priority for an IRQ.
 * Used by gic_inject_virq_lr() to set ICH_LR priority field.
 * Returns 0xA0 (default) if dist is NULL or IRQ not programmed.
 */
u8 vgic_get_priority(const vgic_dist_t *dist, u32 irq);

/*
 * vgic_is_enabled — return true if the IRQ is enabled in the virtual dist.
 * Used by gic_inject_virq_lr() to gate injection.
 */
bool vgic_is_enabled(const vgic_dist_t *dist, u32 irq);

/*
 * vgic_is_masked — return true if irq priority is masked by icc_pmr.
 * priority < icc_pmr → allowed; priority >= icc_pmr → masked.
 * (Lower number = higher priority in ARM GIC)
 */
bool vgic_is_masked(const vgic_cpu_t *cpu, u8 irq_priority);

/* GICD register offset ranges */
#define GICD_OFF_ISENABLER_BASE  0x100u
#define GICD_OFF_ISENABLER_END   0x17Cu
#define GICD_OFF_ICENABLER_BASE  0x180u
#define GICD_OFF_ICENABLER_END   0x1FCu
#define GICD_OFF_IPRIORITY_BASE  0x400u
#define GICD_OFF_IPRIORITY_END   0x4FCu

/* ICC system register encodings (for EC=0x18 ISS decode) */
#define ICC_PMR_EL1_ENC      ((3u<<14)|(0u<<11)|(4u<<7)|(6u<<3)|0u)
#define ICC_BPR0_EL1_ENC     ((3u<<14)|(0u<<11)|(12u<<7)|(8u<<3)|3u)
#define ICC_GRPEN1_EL1_ENC   ((3u<<14)|(0u<<11)|(12u<<7)|(12u<<3)|7u)

#endif /* HYP_VGIC_H */
