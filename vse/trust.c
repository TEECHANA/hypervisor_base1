/*
 * trust.c — VSE Phase 3: Trust Services API implementation
 *
 * Maintains a per-VM trust level and a fault counter. Faults reported via
 * trust_report_fault() escalate the level when thresholds are crossed and
 * enforce by suspending or stopping the offending VM through the existing
 * vm_suspend()/vm_stop() lifecycle calls.
 *
 * Single-core MVP: no locking required. A multi-core port would guard
 * g_trust with a spinlock.
 */

#include "trust.h"
#include "guest_measure.h"
#include "failover.h"   /* VSE Phase 6 */
#include "../core/vm/vm.h"
#include "../include/hypervisor.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

trust_state_t g_trust;

/* ── Internal: monotonic microsecond clock for trust timestamps ──
 *
 * Uses the platform timer at runtime. Under UNIT_TEST it reads a settable
 * global instead, so the auto-promotion clean-period logic can be driven
 * deterministically host-side with no ARM timer / asm dependency.
 */
#ifdef UNIT_TEST
u64 g_trust_test_now_us = 0;               /* test-controlled clock */
static inline u64 _now_us(void) { return g_trust_test_now_us; }
#else
#include "../drivers/timer/timer.h"        /* timer_now_us() */
static inline u64 _now_us(void) { return timer_now_us(); }
#endif

/* ── Internal: validate vm_id and return zero-based index, or -1 ── */
static inline s32 _idx(u32 vm_id)
{
    if (vm_id == 0u || vm_id > MAX_VMS) return -1;
    return (s32)(vm_id - 1u);
}

/* ── trust_level_str ── */
const char *trust_level_str(trust_level_t lvl)
{
    switch (lvl) {
    case TRUST_UNKNOWN:    return "UNKNOWN";
    case TRUST_TRUSTED:    return "TRUSTED";
    case TRUST_DEGRADED:   return "DEGRADED";
    case TRUST_QUARANTINE: return "QUARANTINE";
    case TRUST_REVOKED:    return "REVOKED";
    default:               return "?";
    }
}

/* ── trust_init ──
 *
 * Called after vm_subsys_init(): every VM that was created is marked
 * TRUSTED. VMs not present remain UNKNOWN.
 */
err_t trust_init(void)
{
    memset(&g_trust, 0, sizeof(g_trust));

    for (u32 i = 0; i < MAX_VMS; i++) {
        g_trust.vm[i].level       = TRUST_UNKNOWN;
        g_trust.vm[i].fault_count = 0u;
    }

    u32 marked = 0u;
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = (vm_t *)g_hyp.vms[i];
        if (!vm) continue;
        s32 ix = _idx(vm->id);
        if (ix < 0) continue;
        /* Measure the guest image before granting trust (genuineness check). */
        gmeas_result_t gm = guest_measure_vm(vm->id, vm->name);
        if (gm == GMEAS_MISMATCH) {
            /* Image failed genuineness — do NOT trust it; quarantine instead. */
            g_trust.vm[ix].level = TRUST_QUARANTINE;
            LOG_ERROR("VSE Phase 3: VM%u '%s' -> QUARANTINE (genuineness FAILED)",
                      vm->id, vm->name);
        } else {
            /* MATCH, LEARN, or SKIP → trust (LEARN/SKIP keep prior behaviour
             * so provisioning and undescribed VMs still boot). In enforce mode
             * with a provisioned golden, only GMEAS_MATCH reaches here. */
            g_trust.vm[ix].level = TRUST_TRUSTED;
            marked++;
            LOG_INFO("VSE Phase 3: VM%u '%s' -> TRUSTED", vm->id, vm->name);
        }
    }

    g_trust.initialized = true;
    LOG_INFO("VSE Phase 3: trust services ready (%u VMs trusted)", marked);
    return E_OK;
}

/* ── Query ── */

trust_level_t trust_get(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0 || !g_trust.initialized) return TRUST_UNKNOWN;
    return g_trust.vm[ix].level;
}

bool trust_is_ok(u32 vm_id)
{
    trust_level_t l = trust_get(vm_id);
    return (l == TRUST_TRUSTED || l == TRUST_DEGRADED);
}

bool trust_is_blocked(u32 vm_id)
{
    trust_level_t l = trust_get(vm_id);
    return (l == TRUST_QUARANTINE || l == TRUST_REVOKED);
}

/* ── trust_set ── */
err_t trust_set(u32 vm_id, trust_level_t level)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;
    if (!g_trust.initialized) return E_INVAL;

    trust_level_t old = g_trust.vm[ix].level;
    if (old != level) {
        g_trust.vm[ix].level = level;
        g_trust.vm[ix].downgrade_time = _now_us();
        LOG_INFO("VSE Phase 3: VM%u trust %s -> %s",
                 vm_id, trust_level_str(old), trust_level_str(level));
    }
    return E_OK;
}

/* ── trust_quarantine ── */
err_t trust_quarantine(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;

    vm_t *vm = vm_by_id(vm_id);
    if (!vm) return E_NOTFOUND;

    if (g_trust.vm[ix].level >= TRUST_QUARANTINE) {
        return E_OK;   /* already quarantined or worse */
    }

    LOG_ERROR("VSE Phase 3: QUARANTINING VM%u '%s'", vm_id, vm->name);
    trust_set(vm_id, TRUST_QUARANTINE);
    g_trust.quarantine_count++;

    /* Suspend the VM if it is currently runnable */
    if (vm->state == VM_RUNNING || vm->state == VM_READY) {
        err_t e = vm_suspend(vm);
        if (FAIL(e))
            LOG_WARN("VSE Phase 3: vm_suspend(VM%u) failed (err=%d)",
                     vm_id, (int)e);
    }

    /*
     * VSE Phase 6: if this VM is registered for failover, attempt to
     * restart it from its backup OS. If no backup is registered,
     * failover_on_quarantine() returns E_NOTFOUND and the VM stays
     * suspended (the default Phase-3 behaviour).
     */
    failover_on_quarantine(vm_id);

    return E_OK;
}

/* ── trust_revoke ── */
err_t trust_revoke(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;

    vm_t *vm = vm_by_id(vm_id);
    if (!vm) return E_NOTFOUND;

    LOG_ERROR("VSE Phase 3: REVOKING VM%u '%s'", vm_id, vm->name);
    trust_set(vm_id, TRUST_REVOKED);

    if (vm->state != VM_STOPPED) {
        err_t e = vm_stop(vm);
        if (FAIL(e))
            LOG_WARN("VSE Phase 3: vm_stop(VM%u) failed (err=%d)",
                     vm_id, (int)e);
    }
    return E_OK;
}

/* ── trust_restore ──
 *
 * Bring a quarantined VM back to DEGRADED (not full TRUSTED — it has a
 * history) and resume it. Resets the fault counter so it gets a fresh
 * escalation window.
 */
err_t trust_restore(u32 vm_id)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;

    vm_t *vm = vm_by_id(vm_id);
    if (!vm) return E_NOTFOUND;

    if (g_trust.vm[ix].level != TRUST_QUARANTINE) {
        LOG_WARN("VSE Phase 3: VM%u not quarantined — restore ignored", vm_id);
        return E_INVAL;
    }

    LOG_INFO("VSE Phase 3: restoring VM%u '%s' to DEGRADED", vm_id, vm->name);
    g_trust.vm[ix].fault_count = 0u;
    trust_set(vm_id, TRUST_DEGRADED);

    if (vm->state == VM_SUSPENDED) {
        err_t e = vm_resume(vm);
        if (FAIL(e))
            LOG_WARN("VSE Phase 3: vm_resume(VM%u) failed (err=%d)",
                     vm_id, (int)e);
    }
    return E_OK;
}

/* ── trust_report_fault ──
 *
 * The main feed-in point. Increments the fault counter and auto-escalates:
 *   count >= QUARANTINE threshold -> trust_quarantine()
 *   count >= DEGRADED   threshold -> DEGRADED
 */
err_t trust_report_fault(u32 vm_id, u32 fault_type, u64 detail)
{
    s32 ix = _idx(vm_id);
    if (ix < 0) return E_INVAL;
    if (!g_trust.initialized) return E_INVAL;

    trust_record_t *r = &g_trust.vm[ix];

    /* A revoked VM is terminal — ignore further faults */
    if (r->level == TRUST_REVOKED) return E_OK;

    r->fault_count++;
    r->last_fault_type   = fault_type;
    r->last_fault_detail = detail;
    r->last_fault_us     = _now_us();   /* for the auto-promote clean period */
    g_trust.total_faults++;

    LOG_WARN("VSE Phase 3: VM%u fault type=%x detail=%lx (count=%u)",
             vm_id, fault_type, detail, r->fault_count);

    /* Escalate based on cumulative count */
    if (r->fault_count >= TRUST_THRESH_QUARANTINE) {
        if (r->level < TRUST_QUARANTINE)
            trust_quarantine(vm_id);
    } else if (r->fault_count >= TRUST_THRESH_DEGRADED) {
        if (r->level == TRUST_TRUSTED)
            trust_set(vm_id, TRUST_DEGRADED);
    }
    return E_OK;
}

/* ── trust_auto_promote_tick ──
 *
 * Periodic upgrade scan. A VM that was downgraded to DEGRADED (by faults) and
 * has since stayed quiet for TRUST_CLEAN_PERIOD_US is promoted back to TRUSTED
 * and its fault counter cleared. Only DEGRADED is auto-promoted: QUARANTINE and
 * REVOKED require explicit restore/policy (a quarantined VM must be recovered,
 * not silently re-trusted). Iterates g_trust directly (no vm_t dependency).
 */
u32 trust_auto_promote_tick(u64 now_us)
{
    if (!g_trust.initialized) return 0u;

    u32 promoted = 0u;
    for (u32 i = 0; i < MAX_VMS; i++) {
        trust_record_t *r = &g_trust.vm[i];
        if (r->level != TRUST_DEGRADED) continue;

        /* Guard against a not-yet-stamped record and clock going backwards. */
        if (now_us < r->last_fault_us) continue;
        if ((now_us - r->last_fault_us) < (u64)TRUST_CLEAN_PERIOD_US) continue;

        r->fault_count = 0u;
        trust_set(i + 1u, TRUST_TRUSTED);
        LOG_INFO("VSE Phase 3: VM%u trust DEGRADED -> TRUSTED "
                 "(clean period elapsed)", i + 1u);
        promoted++;
    }
    return promoted;
}

/* ── trust_print_status ── */
void trust_print_status(void)
{
    LOG_INFO("=== VSE Phase 3: Trust Status ===");
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = (vm_t *)g_hyp.vms[i];
        if (!vm) continue;
        s32 ix = _idx(vm->id);
        if (ix < 0) continue;
        trust_record_t *r = &g_trust.vm[ix];
        LOG_INFO("  VM%u %-8s %-10s faults=%u lastType=%x",
                 vm->id, vm->name,
                 trust_level_str(r->level), r->fault_count,
                 r->last_fault_type);
    }
    LOG_INFO("  total_faults=%u quarantines=%u",
             g_trust.total_faults, g_trust.quarantine_count);
    LOG_INFO("================================");
}
