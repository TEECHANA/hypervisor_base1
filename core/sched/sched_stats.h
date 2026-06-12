/*
 * sched_stats.h — Per-VM CPU time accounting (Phase 3 §3.3.2)
 *
 * Tracks for each VM:
 *   total_us        — total CPU microseconds consumed since boot
 *   slice_count     — number of time slices received
 *   preempt_count   — number of times preempted by timer IRQ
 *   wfi_count       — number of WFI yields (voluntary idle)
 *   current_slice_us— duration of the current running slice
 *
 * Updated on every context switch by the scheduler.
 * Queried via HVC_SCHED_GET_STATS hypercall.
 */

#ifndef HYP_SCHED_STATS_H
#define HYP_SCHED_STATS_H

#include "../../include/types.h"
#include "../../include/error.h"
#include "../../include/config.h"

typedef struct {
    u64 total_us;           /* total CPU time this VM has consumed   */
    u32 slice_count;        /* number of slices allocated            */
    u32 preempt_count;      /* timer-IRQ preemptions                 */
    u32 wfi_count;          /* voluntary WFI yields                  */
    u32 slice_dur_us;       /* current configured slice duration      */
    u32 vm_id;              /* which VM this record belongs to        */
} vm_stats_t;

/* Initialise stats table — called from sched_init() */
void sched_stats_init(void);

/* Called on every context switch (from sched_on_timer / sched_on_wfi) */
void sched_stats_on_switch(u32 prev_vm_id, u32 next_vm_id,
                            u64 slice_us, bool was_preempt);

/* Called when a VM executes WFI (voluntary yield) */
void sched_stats_on_wfi(u32 vm_id);

/* Update configured slice duration for a VM */
void sched_stats_set_slice(u32 vm_id, u32 dur_us);

/* Read stats for a VM — used by HVC_SCHED_GET_STATS */
err_t sched_stats_get(u32 vm_id, vm_stats_t *out);

/* Print a per-VM utilisation report to the hypervisor log */
void sched_stats_report(void);

/* Global switch counter — incremented on every context switch */
extern u64 g_switch_count;

#endif /* HYP_SCHED_STATS_H */
