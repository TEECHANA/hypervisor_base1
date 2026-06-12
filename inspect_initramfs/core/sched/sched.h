#ifndef HYP_SCHED_H
#define HYP_SCHED_H
#include "../../include/types.h"
#include "../../include/error.h"
#include "../../include/config.h"

typedef struct {
    u32 vm_id;
    u32 vcpu_id;
    u32 duration_us;
} sched_slot_t;

err_t sched_init(void);
void  sched_run(void)                  __noreturn;
void  sched_secondary_run(u32 cpu_id) __noreturn;
void  sched_on_timer(void);            /* called from GIC IRQ handler */
void  sched_add_slot(u32 vm_id, u32 vcpu_id, u32 dur_us);
#endif
