/*
 * ids.c — VSE IDS implementation (see ids.h for the design rationale)
 *
 * Read-only against the existing VSE state (g_trust, fdetect stats). No
 * dynamic allocation. Single-core MVP — no locking (matches trust.c). A hook
 * here must NEVER panic: a crashing detector is worse than the intrusion.
 */

#include "ids.h"
#include "trust.h"
#include "fault_detect.h"
#include "../drivers/timer/timer.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

ids_state_t g_ids;

/* ── vm_id → zero-based index, or -1 ── */
static inline s32 _idx(u32 vm_id)
{
    if (vm_id == 0u || vm_id > MAX_VMS) return -1;
    return (s32)(vm_id - 1u);
}

/* ── append a record to the audit ring buffer ── */
static void _log(u32 vm_id, ids_event_type_t type, ids_severity_t sev,
                 u32 fault_type, u64 detail)
{
    ids_record_t *r = &g_ids.log[g_ids.log_head];
    r->time_us    = timer_now_us();
    r->vm_id      = vm_id;
    r->type       = type;
    r->severity   = sev;
    r->fault_type = fault_type;
    r->detail     = detail;
    r->trust_at   = (vm_id ? trust_get(vm_id) : TRUST_UNKNOWN);

    g_ids.log_head = (g_ids.log_head + 1u) % IDS_LOG_CAPACITY;
    g_ids.log_count++;

    if (sev == IDS_SEV_ALERT) g_ids.alerts++;
}

/* ── string helpers ── */
const char *ids_event_str(ids_event_type_t t)
{
    switch (t) {
    case IDS_EV_FAULT:      return "FAULT";
    case IDS_EV_STORM:      return "STORM";
    case IDS_EV_RAPID:      return "RAPID-ESC";
    case IDS_EV_REPEAT:     return "REPEAT";
    case IDS_EV_TRUST_DROP: return "TRUST-DROP";
    case IDS_EV_POLL:       return "POLL";
    case IDS_EV_ENFORCE:    return "ENFORCE";
    default:                return "NONE";
    }
}

const char *ids_sev_str(ids_severity_t s)
{
    switch (s) {
    case IDS_SEV_INFO:  return "INFO";
    case IDS_SEV_WARN:  return "WARN";
    case IDS_SEV_ALERT: return "ALERT";
    default:            return "?";
    }
}

/* ── ids_init ── */
err_t ids_init(void)
{
    memset(&g_ids, 0, sizeof(g_ids));

    u64 now = timer_now_us();
    for (u32 i = 0; i < MAX_VMS; i++) {
        g_ids.mon[i].window_start_us = now;
        g_ids.mon[i].first_fault_us  = 0;
        g_ids.mon[i].last_seen_level = trust_get(i + 1u);
    }
    g_ids.last_poll_us  = now;
    g_ids.initialized   = true;

    LOG_INFO("VSE IDS: monitor ready "
             "(storm>%u/%lums, rapid<%lums, repeat>=%u, log=%u)",
             IDS_STORM_FAULTS, IDS_STORM_WINDOW_US / 1000ULL,
             IDS_RAPID_US / 1000ULL, IDS_REPEAT_COUNT, IDS_LOG_CAPACITY);
    return E_OK;
}

/* ── ids_notify_fault ──
 *
 * Real-time analysis on each reported fault. Runs the three detectors:
 * storm (rate), repeat (same type run), rapid (fast first-fault→quarantine).
 */
void ids_notify_fault(u32 vm_id, u32 fault_type, u64 detail)
{
    if (!g_ids.initialized) return;
    s32 ix = _idx(vm_id);
    if (ix < 0) return;

    ids_vm_mon_t *m = &g_ids.mon[ix];
    u64 now = timer_now_us();

    /* Always record the raw fault as evidence. */
    _log(vm_id, IDS_EV_FAULT, IDS_SEV_WARN, fault_type, detail);

    /* First fault since the VM was last clean → start the rapid-escalation clock. */
    if (m->first_fault_us == 0)
        m->first_fault_us = now;

    /* ── REPEAT detection: same fault type in a row ── */
    if (fault_type == m->last_fault_type) {
        m->repeat_run++;
        if (m->repeat_run == IDS_REPEAT_COUNT) {
            g_ids.anomalies++;
            _log(vm_id, IDS_EV_REPEAT, IDS_SEV_ALERT, fault_type,
                 (u64)m->repeat_run);
            LOG_ERROR("VSE IDS: ALERT VM%u repeated fault type=%x x%u",
                      vm_id, fault_type, m->repeat_run);
        }
    } else {
        m->repeat_run    = 1u;
        m->last_fault_type = fault_type;
    }

    /* ── STORM detection: too many faults in a sliding window ── */
    if (now - m->window_start_us > IDS_STORM_WINDOW_US) {
        /* window expired — reset */
        m->window_start_us = now;
        m->window_faults   = 1u;
    } else {
        m->window_faults++;
        if (m->window_faults == IDS_STORM_FAULTS + 1u) {
            g_ids.anomalies++;
            _log(vm_id, IDS_EV_STORM, IDS_SEV_ALERT, fault_type,
                 (u64)m->window_faults);
            LOG_ERROR("VSE IDS: ALERT VM%u fault STORM %u faults in <%lums",
                      vm_id, m->window_faults, IDS_STORM_WINDOW_US / 1000ULL);

#if IDS_ENFORCE
            /*
             * Active enforcement (Activity 1.2.2): a STORM is treated as a
             * critical anomaly. The IDS quarantines the VM directly rather
             * than waiting for the trust engine's fault-count threshold.
             * trust_quarantine() suspends the VM and (Phase 6) triggers
             * failover to its backup OS if registered.
             *
             * Guard: only act if the VM isn't already quarantined/revoked,
             * so we enforce once per incident, not on every storm fault.
             */
            if (!trust_is_blocked(vm_id)) {
                LOG_ERROR("VSE IDS: ENFORCING — quarantining VM%u (STORM)", vm_id);
                err_t qe = trust_quarantine(vm_id);
                g_ids.enforcements++;
                _log(vm_id, IDS_EV_ENFORCE, IDS_SEV_ALERT, fault_type,
                     (u64)m->window_faults);
                if (FAIL(qe))
                    LOG_WARN("VSE IDS: trust_quarantine(VM%u) returned err=%d",
                             vm_id, (int)qe);
            }
#endif
        }
    }

    /* ── RAPID escalation: VM hit QUARANTINE quickly after first fault ── */
    if (trust_get(vm_id) >= TRUST_QUARANTINE &&
        m->first_fault_us != 0 &&
        (now - m->first_fault_us) < IDS_RAPID_US) {
        g_ids.anomalies++;
        _log(vm_id, IDS_EV_RAPID, IDS_SEV_ALERT, fault_type,
             now - m->first_fault_us);
        LOG_ERROR("VSE IDS: ALERT VM%u RAPID escalation to QUARANTINE in %lums",
                  vm_id, (now - m->first_fault_us) / 1000ULL);
        /* avoid re-firing every subsequent fault */
        m->first_fault_us = 0;
    }
}

/* ── ids_poll ──
 *
 * Periodic heartbeat + downgrade watch. Returns number of new anomalies.
 */
u32 ids_poll(void)
{
    if (!g_ids.initialized) return 0;

    u64 now = timer_now_us();
    u32 new_anom = 0;

    /* Detect any trust downgrade since the previous poll. */
    for (u32 i = 0; i < MAX_VMS; i++) {
        trust_level_t cur = trust_get(i + 1u);
        trust_level_t prev = g_ids.mon[i].last_seen_level;
        if (cur > prev && prev != TRUST_UNKNOWN) {
            /* higher enum value = less trusted = a downgrade */
            g_ids.anomalies++;
            new_anom++;
            _log(i + 1u, IDS_EV_TRUST_DROP, IDS_SEV_ALERT, 0,
                 ((u64)prev << 8) | (u64)cur);
            LOG_WARN("VSE IDS: VM%u trust dropped %s -> %s",
                     i + 1u, trust_level_str(prev), trust_level_str(cur));
        }
        g_ids.mon[i].last_seen_level = cur;
    }

    /* Symmetric upgrade scan: promote DEGRADED VMs that have stayed clean long
     * enough back to TRUSTED (Phase 3 auto-recovery). Uses the same `now`. */
    trust_auto_promote_tick(now);

    /* Heartbeat record (carries the global fault total as detail). */
    _log(0u, IDS_EV_POLL, IDS_SEV_INFO, 0, (u64)g_trust.total_faults);
    LOG_INFO("VSE IDS: poll heartbeat — total_faults=%u new_anom=%u",
             (u32)g_trust.total_faults, new_anom);
    g_ids.last_poll_us = now;

    return new_anom;
}

/* ── Query ── */
u32 ids_log_count(void)
{
    return (g_ids.log_count < IDS_LOG_CAPACITY)
         ? g_ids.log_count : IDS_LOG_CAPACITY;
}

const ids_record_t *ids_log_at(u32 i)
{
    u32 n = ids_log_count();
    if (i >= n) return (const ids_record_t *)0;

    /* Oldest-first ordering. When wrapped, oldest is at log_head. */
    u32 start = (g_ids.log_count <= IDS_LOG_CAPACITY)
              ? 0u : g_ids.log_head;
    u32 idx = (start + i) % IDS_LOG_CAPACITY;
    return &g_ids.log[idx];
}

/* ── Diagnostics ── */
void ids_print_log(void)
{
    u32 n = ids_log_count();
    LOG_INFO("=== VSE IDS: Audit Log (%u records) ===", n);
    for (u32 i = 0; i < n; i++) {
        const ids_record_t *r = ids_log_at(i);
        if (!r) continue;
        LOG_INFO("  [%lu us] VM%u %s %s ft=%x detail=%lx trust=%s",
                 r->time_us, r->vm_id, ids_event_str(r->type),
                 ids_sev_str(r->severity), r->fault_type, r->detail,
                 trust_level_str(r->trust_at));
    }
    LOG_INFO("=======================================");
}

void ids_print_summary(void)
{
    fdetect_stats_t fs;
    fdetect_get_stats(&fs);
    LOG_INFO("=== VSE IDS: Summary ===");
    LOG_INFO("  records=%u anomalies=%u alerts=%u enforcements=%u",
             ids_log_count(), g_ids.anomalies, g_ids.alerts, g_ids.enforcements);
    LOG_INFO("  faults: mem=%u dma=%u irq=%u total=%u",
             fs.mem_faults, fs.dma_violations, fs.irq_unrouted, fs.total);
    for (u32 i = 0; i < MAX_VMS; i++) {
        trust_level_t l = trust_get(i + 1u);
        if (l == TRUST_UNKNOWN) continue;
        LOG_INFO("  VM%u trust=%s faults=%u",
                 i + 1u, trust_level_str(l), g_trust.vm[i].fault_count);
    }
    LOG_INFO("========================");
}
