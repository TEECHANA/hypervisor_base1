/*
 * trust.h — VSE Phase 3: Trust Services API
 *
 * Phases 1 and 2 proved the HYPERVISOR itself is trustworthy (its config
 * and its code are unmodified). Phase 3 tracks how much each GUEST can be
 * trusted at runtime.
 *
 * Every VM is assigned a trust level. The level starts at TRUSTED once the
 * VM is created and measured, and can be downgraded when the system detects
 * misbehavior (faults, violations — fed in by Phase 5 fault detection and,
 * later, the IDS). Downgrades drive enforcement: a QUARANTINED VM is
 * suspended; a REVOKED VM is stopped.
 *
 * This phase provides the state machine and the API. It does NOT itself
 * decide when to downgrade — that policy is driven by callers
 * (vse_trust_report_fault) so later phases can plug in without changing
 * this module.
 *
 * Boot position (in hyp_main, AFTER vm_subsys_init so VMs exist):
 *   vm_subsys_init()
 *   → trust_init()        ← THIS MODULE: marks all created VMs TRUSTED
 *   → sched_init()
 */

#ifndef VSE_TRUST_H
#define VSE_TRUST_H

#include "../include/types.h"
#include "../include/error.h"
#include "../include/config.h"

/* ── Trust levels (ordered: higher = less trusted) ── */
typedef enum {
    TRUST_UNKNOWN    = 0,   /* not yet measured / no VM */
    TRUST_TRUSTED    = 1,   /* normal operation */
    TRUST_DEGRADED   = 2,   /* anomalies seen — heightened monitoring */
    TRUST_QUARANTINE = 3,   /* suspended, under investigation */
    TRUST_REVOKED    = 4,   /* terminated, resources reclaimed */
} trust_level_t;

/* ── Fault categories reported into the trust engine ── */
#define TRUST_FAULT_MEM      0x01u  /* unauthorized / cross-VM memory access */
#define TRUST_FAULT_DMA      0x02u  /* DMA boundary violation */
#define TRUST_FAULT_IRQ      0x03u  /* invalid / spoofed IRQ activity */
#define TRUST_FAULT_HVC      0x04u  /* hypercall abuse */
#define TRUST_FAULT_SCHED    0x05u  /* scheduler manipulation */
#define TRUST_FAULT_GENERIC  0xFFu  /* unspecified */

/* ── Escalation thresholds (fault count per VM) ── */
#ifndef TRUST_THRESH_DEGRADED
#define TRUST_THRESH_DEGRADED    5u
#endif
#ifndef TRUST_THRESH_QUARANTINE
#define TRUST_THRESH_QUARANTINE  15u
#endif

/* ── Auto-promotion: a DEGRADED VM with no new fault for this long (in
 * microseconds, timer_now_us() base) is promoted back to TRUSTED. Overridable
 * at build time (the runtime self-test shortens it). Default 30 s. ── */
#ifndef TRUST_CLEAN_PERIOD_US
#define TRUST_CLEAN_PERIOD_US    30000000ull
#endif

/* ── Per-VM trust record ── */
typedef struct {
    trust_level_t level;
    u32           fault_count;     /* cumulative faults reported */
    u32           last_fault_type; /* most recent TRUST_FAULT_* */
    u64           last_fault_detail;
    u64           downgrade_time;  /* time (us) of last level change */
    u64           last_fault_us;   /* time (us) of most recent fault */
} trust_record_t;

/* ── Trust engine state (indexed by vm_id-1) ── */
typedef struct {
    bool           initialized;
    trust_record_t vm[MAX_VMS];
    u32            total_faults;
    u32            quarantine_count;
} trust_state_t;

extern trust_state_t g_trust;

/* ── Lifecycle ── */
err_t trust_init(void);          /* mark all existing VMs TRUSTED */

/* ── Query ── */
trust_level_t trust_get(u32 vm_id);
bool          trust_is_ok(u32 vm_id);      /* TRUSTED or DEGRADED */
bool          trust_is_blocked(u32 vm_id); /* QUARANTINE or REVOKED */
const char   *trust_level_str(trust_level_t lvl);

/* ── Mutation ── */
err_t trust_set(u32 vm_id, trust_level_t level);

/*
 * trust_report_fault — record a fault against a VM and auto-escalate.
 *
 * This is the main entry point for Phase 5 fault detection and the IDS.
 * It increments the VM's fault counter and, when thresholds are crossed,
 * downgrades trust (TRUSTED→DEGRADED→QUARANTINE) and enforces:
 *   - QUARANTINE → vm_suspend()
 *   - REVOKED    → vm_stop()
 */
err_t trust_report_fault(u32 vm_id, u32 fault_type, u64 detail);

/*
 * trust_auto_promote_tick — periodic upgrade scan (symmetric to the IDS
 * downgrade watch). Any VM in DEGRADED that has gone TRUST_CLEAN_PERIOD_US
 * with no new fault is promoted back to TRUSTED. `now_us` is the current
 * timer_now_us() value (passed in so the caller owns the clock / so it is
 * mockable under UNIT_TEST). Returns the number of VMs promoted.
 * Called from ids_poll().
 */
u32 trust_auto_promote_tick(u64 now_us);

/* ── Explicit enforcement (callable by policy / operator) ── */
err_t trust_quarantine(u32 vm_id);  /* suspend + mark QUARANTINE */
err_t trust_revoke(u32 vm_id);      /* stop + mark REVOKED */
err_t trust_restore(u32 vm_id);     /* QUARANTINE → DEGRADED, resume */

/* ── Diagnostics ── */
void trust_print_status(void);

#endif /* VSE_TRUST_H */
