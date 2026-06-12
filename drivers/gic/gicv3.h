/*
 * gicv3.h — GICv3 driver interface
 *
 * Phase 1: gic_inject_virq_lr() exported so irq_router can use it
 * without hard-coding a specific LR index.
 */

#ifndef GICV3_H
#define GICV3_H

#include "../../include/types.h"
#include "../../include/error.h"

err_t gic_init         (u64 gicd_base, u64 gicr_base);
void  gic_handle_irq   (void *regs);
void  gic_enable_irq   (u32 irq);
void  gic_disable_irq  (u32 irq);

/* Inject into LR0 — used for the virtual timer (must be stable in LR0) */
void  gic_inject_virq    (u32 virq, u32 prio);

/* Phase 1: inject into the first free LR — use for all non-timer vIRQs */
void  gic_inject_virq_lr (u32 virq, u32 prio);

/* MMIO emulation handler for GICD passthrough */
err_t gic_dist_emulate (u64 addr, bool write, u64 *val, void *priv);

#endif /* GICV3_H */
