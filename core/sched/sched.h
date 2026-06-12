/*
 * sched.h — Scheduler API (Phase 3)
 *
 * Phase 3 additions:
 *   sched_set_slice()  — change a VM's time slice at runtime
 *   sched_get_stats()  — query per-VM CPU stats (used by HVC handler)
 *   sched_print_ctx()  — log current VM on every switch (visibility)
 */

#ifndef HYP_SCHED_H
#define HYP_SCHED_H

#include "../../include/types.h"
#include "../../include/error.h"
#include "../../include/config.h"
#include "sched_stats.h"

typedef struct { u32 vm_id; u32 vcpu_id; u32 duration_us; } sched_slot_t;

/* Core scheduler lifecycle */
err_t sched_init        (void);
void  sched_run         (void) __noreturn;
void  sched_secondary_run(u32 cpu_id) __noreturn;

/* Called from interrupt/trap handlers */
void  sched_on_timer    (void);   /* CNTHP timer IRQ          */
void  sched_on_wfi      (void);   /* EC_WFI sync trap         */

/* Slot management */
void  sched_add_slot    (u32 vm_id, u32 vcpu_id, u32 dur_us);

/*
 * Phase 3: runtime slice configuration.
 * Changes the time slice for vm_id. Takes effect on the next switch.
 * Returns E_NOTFOUND if no slot exists for vm_id.
 */
err_t sched_set_slice   (u32 vm_id, u32 dur_us);

/*
 * Phase 3: query per-VM CPU stats.
 * Fills *out with the stats for vm_id.
 */
err_t sched_get_stats   (u32 vm_id, vm_stats_t *out);

/* g_sched_stopped: set to true to halt the scheduler (used in tests) */
extern bool g_sched_stopped;

#endif /* HYP_SCHED_H */
