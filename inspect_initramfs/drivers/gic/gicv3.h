#ifndef HYP_GICV3_H
#define HYP_GICV3_H
#include "../../include/types.h"
#include "../../include/error.h"
err_t gic_init(u64 gicd, u64 gicr);
void  gic_enable_irq(u32 irq);
void  gic_disable_irq(u32 irq);
void  gic_inject_virq(u32 virq, u32 prio);
void  gic_handle_irq(void *regs);
#endif
