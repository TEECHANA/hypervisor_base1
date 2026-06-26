/*
 * ids.h — VSE IDS: Intrusion Detection System (Activity 1.2.1)
 *
 * What this adds on top of the existing VSE
 * -----------------------------------------
 * Phase 3 (trust.c) + Phase 5 (fault_detect.c) already DETECT hard faults
 * (Stage-2 violations, DMA boundary breaks, unrouted IRQs) and RESPOND by
 * counting them per-VM and auto-quarantining at fixed thresholds.
 *
 * The IDS adds the three things the activity tracker asks for that the trust
 * engine alone does not provide:
 *
 *   1. RATE / PATTERN detection (not just a cumulative count). The trust
 *      engine treats 5 faults over an hour and 5 faults in 1 ms identically.
 *      A burst is a far stronger attack signal. The IDS measures faults per
 *      time window using timer_now_us() and raises anomalies on:
 *        - fault STORMS (too many faults in a short window),
 *        - RAPID escalation (TRUSTED→QUARANTINE faster than N us),
 *        - REPEATED same-type faults (a VM hammering one violation class).
 *
 *   2. An AUDIT LOG: a ring buffer of timestamped, queryable events
 *      ("traceable evidence supporting security analysis / forensic
 *      investigation"). Each record has time, vm_id, event class, detail,
 *      and the trust level at the time. The Python GUI (Activity 1.2.3)
 *      reads these.
 *
 *   3. CONTINUOUS MONITORING: ids_poll() is meant to be called periodically
 *      (Activity Tracker Item 4: "monitor the guest OS every 10 sec"). Each
 *      poll snapshots trust + fault state and emits an audit record, so the
 *      GUI sees a heartbeat even when nothing is wrong.
 *
 * Integration model: READ-ONLY against the existing VSE. The IDS does not
 * modify trust.c or fault_detect.c. It reads the public g_trust state and
 * fdetect_get_stats(). The fault hooks MAY additionally call ids_notify_fault()
 * to give the IDS immediate (not just polled) visibility — see ids.c. This
 * matches the trust.h design promise that later phases "plug in without
 * changing this module."
 */

#ifndef VSE_IDS_H
#define VSE_IDS_H

#include "../include/types.h"
#include "../include/error.h"
#include "../include/config.h"
#include "trust.h"

/* ── Event classes recorded in the audit log ── */
typedef enum {
    IDS_EV_NONE       = 0,
    IDS_EV_FAULT      = 1,  /* a fault was reported (mirrors a trust fault)   */
    IDS_EV_STORM      = 2,  /* fault rate exceeded the storm threshold        */
    IDS_EV_RAPID      = 3,  /* trust dropped very fast (rapid escalation)     */
    IDS_EV_REPEAT     = 4,  /* same fault type repeated beyond threshold      */
    IDS_EV_TRUST_DROP = 5,  /* observed a VM trust-level downgrade            */
    IDS_EV_POLL       = 6,  /* periodic monitor heartbeat                     */
    IDS_EV_ENFORCE    = 7,  /* IDS actively quarantined a VM (active policy)  */
} ids_event_type_t;

/* ── Severity, for the GUI to colour-code ── */
typedef enum {
    IDS_SEV_INFO  = 0,
    IDS_SEV_WARN  = 1,
    IDS_SEV_ALERT = 2,
} ids_severity_t;

/* ── One audit-log record ── */
typedef struct {
    u64              time_us;     /* timer_now_us() at record time            */
    u32              vm_id;       /* 0 = hypervisor/global                    */
    ids_event_type_t type;
    ids_severity_t   severity;
    u32              fault_type;  /* TRUST_FAULT_* if applicable, else 0      */
    u64              detail;      /* event-specific (IPA, rate, etc.)         */
    trust_level_t    trust_at;    /* VM trust level when recorded             */
} ids_record_t;

/* ── Ring-buffer capacity (power of two not required) ── */
#ifndef IDS_LOG_CAPACITY
#define IDS_LOG_CAPACITY   128u
#endif

/* ── Detection tunables ──
 *
 * STORM: more than IDS_STORM_FAULTS faults from one VM within
 *        IDS_STORM_WINDOW_US microseconds.
 * RAPID: a VM reaching QUARANTINE within IDS_RAPID_US of its first fault.
 * REPEAT: the same fault_type seen IDS_REPEAT_COUNT times in a row.
 */
#ifndef IDS_STORM_FAULTS
#define IDS_STORM_FAULTS     4u
#endif
#ifndef IDS_STORM_WINDOW_US
#define IDS_STORM_WINDOW_US  1000000ULL   /* 1 second */
#endif
#ifndef IDS_RAPID_US
#define IDS_RAPID_US         2000000ULL   /* 2 seconds first-fault→quarantine */
#endif
#ifndef IDS_REPEAT_COUNT
#define IDS_REPEAT_COUNT     3u
#endif

/*
 * ── Active enforcement policy (Activity 1.2.2) ──
 *
 * When IDS_ENFORCE is 1, the IDS does not merely observe — on a critical
 * anomaly (a fault STORM) it directly calls trust_quarantine() on the
 * offending VM. trust_quarantine() suspends the VM and, if the VM is
 * registered for failover (Phase 6), triggers a restart from its backup OS.
 * This makes the IDS an active responder, and exercises the full chain:
 *
 *     IDS detects STORM  ->  IDS quarantines VM  ->  Phase 6 failover
 *
 * Set to 0 to keep the IDS observe-only (detection + logging), letting the
 * trust engine's own fault-count thresholds handle escalation. Observe-only
 * is the more conservative design (detection and enforcement stay separate);
 * active enforcement is the more demonstrable one. Default ON for the demo.
 */
#ifndef IDS_ENFORCE
#define IDS_ENFORCE          1
#endif

/* ── Per-VM monitoring window state ── */
typedef struct {
    u64 window_start_us;     /* start of the current rate window            */
    u32 window_faults;       /* faults counted in the current window        */
    u64 first_fault_us;      /* time of first fault since TRUSTED            */
    u32 last_fault_type;     /* for repeat detection                        */
    u32 repeat_run;          /* consecutive same-type faults                */
    trust_level_t last_seen_level; /* to detect downgrades during poll      */
} ids_vm_mon_t;

/* ── IDS engine state ── */
typedef struct {
    bool          initialized;
    ids_record_t  log[IDS_LOG_CAPACITY];
    u32           log_head;       /* next write index (wraps)                */
    u32           log_count;      /* total records ever written              */
    ids_vm_mon_t  mon[MAX_VMS];
    u32           anomalies;      /* total anomalies (storm+rapid+repeat)    */
    u32           alerts;         /* records emitted at ALERT severity       */
    u32           enforcements;   /* times the IDS actively quarantined a VM */
    u64           last_poll_us;
} ids_state_t;

extern ids_state_t g_ids;

/* ── Lifecycle ── */
/*
 * ids_init — reset the engine. Call at boot AFTER trust_init() and
 * fault_detect_init() so it can read their state.
 */
err_t ids_init(void);

/* ── Real-time feed (optional but recommended) ──
 *
 * ids_notify_fault — called from the Phase 5 hooks (fdetect_*), right where
 * they already call trust_report_fault(). Gives the IDS immediate visibility
 * to run rate/repeat/storm analysis on each fault. Safe to omit (the poll
 * loop still catches trust drops), but adding it makes detection real-time.
 * NEVER panics.
 */
void ids_notify_fault(u32 vm_id, u32 fault_type, u64 detail);

/* ── Periodic monitor ──
 *
 * ids_poll — call on a fixed interval (target: every 10 s, Activity Item 4).
 * Snapshots each VM's trust level, detects any downgrade since the last poll,
 * and emits an IDS_EV_POLL heartbeat record. Returns the number of NEW
 * anomalies found during this poll.
 */
u32 ids_poll(void);

/* ── Query / diagnostics ── */
u32                 ids_log_count(void);        /* records available (<=cap)  */
const ids_record_t *ids_log_at(u32 i);          /* i in [0, ids_log_count())  */
const char         *ids_event_str(ids_event_type_t t);
const char         *ids_sev_str(ids_severity_t s);
void                ids_print_log(void);         /* dump audit log to UART     */
void                ids_print_summary(void);     /* counts + per-VM status     */

#endif /* VSE_IDS_H */
