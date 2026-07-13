/*
 * test_trust_promote.c — unit coverage for GAP E trust auto-promotion.
 *
 * Compiles the REAL vse/trust.c host-side (built with -DUNIT_TEST, which swaps
 * the ARM counter for a settable software clock, g_trust_test_now_us) and drives
 * the DEGRADED -> TRUSTED promotion deterministically:
 *   - threshold faults take a VM to DEGRADED,
 *   - trust_auto_promote_tick() does NOT promote before the clean period,
 *   - it DOES promote once the clean period has elapsed,
 *   - QUARANTINE is never auto-promoted.
 *
 * Single translation unit (like test_string.c): the file #includes trust.c and
 * defines linkable stubs for the deps trust.c references but this path does not
 * exercise. Run via `make test-unit`.
 */
#include <stdio.h>

#include "../../vse/trust.c"   /* code under test (UNIT_TEST software clock) */

/* ── Linkable stubs: referenced by trust.c's quarantine/revoke/init paths,
 *    which this test never calls — they only need to resolve at link time. ── */
hypervisor_t g_hyp;
vm_t *vm_by_id(u32 id)                 { (void)id; return NULL; }
err_t vm_stop(vm_t *vm)                { (void)vm; return E_OK; }
err_t vm_suspend(vm_t *vm)             { (void)vm; return E_OK; }
err_t vm_resume(vm_t *vm)              { (void)vm; return E_OK; }
err_t failover_on_quarantine(u32 id)   { (void)id; return E_OK; }
gmeas_result_t guest_measure_vm(u32 id, const char *name)
                                       { (void)id; (void)name; return GMEAS_SKIP; }

extern u64 g_trust_test_now_us;        /* the mock clock defined in trust.c */

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok  : %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void)
{
    /* Bring the engine up without trust_init() (avoids the VM-table walk). */
    memset(&g_trust, 0, sizeof(g_trust));
    g_trust.initialized = true;
    g_trust.vm[2].level = TRUST_TRUSTED;   /* VM3 -> index 2 */
    g_trust.vm[1].level = TRUST_TRUSTED;   /* VM2 -> index 1 */

    g_trust_test_now_us = 1000000ull;      /* t = 1 s */

    /* DEGRADED-threshold faults take VM3 to DEGRADED (below quarantine). */
    for (u32 i = 0; i < TRUST_THRESH_DEGRADED; i++)
        trust_report_fault(3u, TRUST_FAULT_GENERIC, 0);
    CHECK(trust_get(3u) == TRUST_DEGRADED,
          "VM3 reaches DEGRADED after threshold faults");

    /* Just before the clean period elapses: no promotion. */
    g_trust_test_now_us += (u64)TRUST_CLEAN_PERIOD_US - 1ull;
    u32 p0 = trust_auto_promote_tick(g_trust_test_now_us);
    CHECK(p0 == 0u && trust_get(3u) == TRUST_DEGRADED,
          "no promotion before the clean period elapses");

    /* Once the clean period has elapsed: promote to TRUSTED. */
    g_trust_test_now_us += 2ull;           /* now strictly past the threshold */
    u32 p1 = trust_auto_promote_tick(g_trust_test_now_us);
    CHECK(p1 == 1u && trust_get(3u) == TRUST_TRUSTED,
          "VM3 auto-promoted DEGRADED -> TRUSTED after clean period");
    CHECK(g_trust.vm[2].fault_count == 0u,
          "fault counter reset on promotion (fresh escalation window)");

    /* A fresh fault re-arms the window: a promotion must NOT fire immediately. */
    trust_report_fault(3u, TRUST_FAULT_GENERIC, 0);   /* count=1, still TRUSTED */
    for (u32 i = 1; i < TRUST_THRESH_DEGRADED; i++)
        trust_report_fault(3u, TRUST_FAULT_GENERIC, 0);
    CHECK(trust_get(3u) == TRUST_DEGRADED, "VM3 re-degrades on new fault burst");
    u32 p2 = trust_auto_promote_tick(g_trust_test_now_us);  /* same instant */
    CHECK(p2 == 0u && trust_get(3u) == TRUST_DEGRADED,
          "clean-period window restarts after a new fault");

    /* QUARANTINE is terminal for auto-promotion — only DEGRADED is promoted. */
    g_trust.vm[1].level = TRUST_QUARANTINE;
    g_trust.vm[1].last_fault_us = 0ull;
    g_trust_test_now_us += (u64)TRUST_CLEAN_PERIOD_US * 10ull;
    trust_auto_promote_tick(g_trust_test_now_us);
    CHECK(trust_get(2u) == TRUST_QUARANTINE,
          "QUARANTINE is never auto-promoted");

    printf("Passed with %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
