/*
 * sched_stats.c — Per-VM CPU time accounting (Phase 3 §3.3.2)
 *
 * Every context switch updates two counters:
 *   prev VM: total_us += slice_actually_used, preempt_count++ (if timer)
 *   next VM: slice_count++
 *
 * sched_stats_report() prints a one-line summary per VM to the HYP log.
 * It is called automatically every STATS_REPORT_INTERVAL_SWITCHES switches.
 */

#include "sched_stats.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../include/hypervisor.h"
#include "../../core/vm/vm.h"

/* Print a utilisation report every N context switches */
#define STATS_REPORT_INTERVAL_SWITCHES  100

static vm_stats_t _stats[MAX_VMS];
u64 g_switch_count = 0;

void sched_stats_init(void)
{
    memset(_stats, 0, sizeof(_stats));
    g_switch_count = 0;
    for (u32 i = 0; i < MAX_VMS; i++) {
        _stats[i].vm_id = i + 1;
        _stats[i].slice_dur_us = 10000; /* default 10ms */
    }
    LOG_INFO("Sched stats: initialised for %d VMs", MAX_VMS);
}

void sched_stats_on_switch(u32 prev_vm_id, u32 next_vm_id,
                            u64 slice_us, bool was_preempt)
{
    /* Update prev VM stats */
    if (prev_vm_id >= 1 && prev_vm_id <= MAX_VMS) {
        vm_stats_t *p = &_stats[prev_vm_id - 1];
        p->total_us += slice_us;
        if (was_preempt) p->preempt_count++;
    }

    /* Update next VM stats */
    if (next_vm_id >= 1 && next_vm_id <= MAX_VMS) {
        vm_stats_t *n = &_stats[next_vm_id - 1];
        n->slice_count++;
    }

    g_switch_count++;

    /* Periodic utilisation report */
    if (g_switch_count % STATS_REPORT_INTERVAL_SWITCHES == 0)
        sched_stats_report();
}

void sched_stats_on_wfi(u32 vm_id)
{
    if (vm_id >= 1 && vm_id <= MAX_VMS)
        _stats[vm_id - 1].wfi_count++;
}

void sched_stats_set_slice(u32 vm_id, u32 dur_us)
{
    if (vm_id >= 1 && vm_id <= MAX_VMS)
        _stats[vm_id - 1].slice_dur_us = dur_us;
}

err_t sched_stats_get(u32 vm_id, vm_stats_t *out)
{
    if (!out || vm_id < 1 || vm_id > MAX_VMS) return E_INVAL;
    *out = _stats[vm_id - 1];
    return E_OK;
}

void sched_stats_report(void)
{
    u64 total_all = 0;
    for (u32 i = 0; i < MAX_VMS; i++)
        total_all += _stats[i].total_us;

    u32 sw = (u32)(g_switch_count & 0xFFFFFFFFULL);
    LOG_INFO("=== CPU utilisation (switch #%u) ===", sw);
    for (u32 i = 0; i < MAX_VMS; i++) {
        vm_stats_t *s = &_stats[i];
        vm_t *vm = vm_by_id(s->vm_id);
        if (!vm || vm->state == VM_NONE) continue;

        /* Compute permille (0-1000) to avoid floating point */
        u32 permille = (total_all > 0)
            ? (u32)((s->total_us * 1000ULL) / total_all)
            : 0;

        LOG_INFO("  VM%u %s: %u.%u%%  slices=%u preempt=%u wfi=%u",
                 s->vm_id, vm->name,
                 permille / 10u, permille % 10u,
                 s->slice_count,
                 s->preempt_count,
                 s->wfi_count);
    }
    LOG_INFO("=====================================");
}
