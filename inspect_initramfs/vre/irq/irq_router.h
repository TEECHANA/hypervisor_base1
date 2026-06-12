#ifndef HYP_IRQ_ROUTER_H
#define HYP_IRQ_ROUTER_H
#include "../../include/types.h"
#include "../../include/error.h"
err_t irq_router_init(void);
err_t irq_route_add   (u32 phys, u32 vm_id, u32 virq);
err_t irq_route_remove(u32 phys);
void  irq_route_to_vm (u32 phys);   /* called from GIC driver */
#endif
