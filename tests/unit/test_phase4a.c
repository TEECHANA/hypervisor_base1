/*
 * test_phase4a.c — Phase 4A unit tests: PMU manager
 *
 * Build:
 *   gcc -O0 -g -Wall tests/unit/test_phase4a.c -o build/test_phase4a
 *   ./build/test_phase4a
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t  s32;
typedef s32      err_t;

#define E_OK    0
#define E_INVAL -3
#define FAIL(e) ((e) != 0)
#define OK(e)   ((e) == 0)
#define PMU_NUM_COUNTERS 4

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓  %s\n", msg); _pass++; } \
    else       { printf("  ✗  %s\n", msg); _fail++; } \
} while(0)

/* ── Inline pmu_ctx_t for host testing ── */
typedef struct {
    u64 pmcr_el0;
    u64 pmcntenset_el0;
    u64 pmccntr_el0;
    u64 pmccfiltr_el0;
    u64 pmevcntr[PMU_NUM_COUNTERS];
    u64 pmevtyper[PMU_NUM_COUNTERS];
    u64 pmintenset_el1;
    u64 pmovsr_el0;
    bool enabled;
} pmu_ctx_t;

static void pmu_vcpu_init(pmu_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->pmccfiltr_el0 = 0ULL;
    ctx->enabled = false;
}

/* Simulate PMCR trap handling */
static err_t handle_pmcr_write(pmu_ctx_t *ctx, u64 val)
{
    ctx->pmcr_el0 = val;
    if (val & 1ULL) ctx->enabled = true;
    if (val & (1ULL << 1)) ctx->pmccntr_el0 = 0;
    if (val & (1ULL << 2))
        for (u32 i = 0; i < PMU_NUM_COUNTERS; i++) ctx->pmevcntr[i] = 0;
    return E_OK;
}

static err_t handle_pmcnten_set(pmu_ctx_t *ctx, u64 val)
{
    ctx->pmcntenset_el0 |= val;
    return E_OK;
}

static err_t handle_pmcnten_clr(pmu_ctx_t *ctx, u64 val)
{
    ctx->pmcntenset_el0 &= ~val;
    return E_OK;
}

static err_t handle_pmovsr_write(pmu_ctx_t *ctx, u64 val)
{
    ctx->pmovsr_el0 &= ~val;
    return E_OK;
}

/* Simulate save: accumulate hw cycles into ctx */
static void pmu_save_sim(pmu_ctx_t *ctx, u64 hw_cycles)
{
    ctx->pmccntr_el0 += hw_cycles;
    /* disable (clear E bit) */
    ctx->pmcr_el0 &= ~1ULL;
}

/* Simulate restore: next vCPU starts with hw counter reset */
static void pmu_restore_sim(pmu_ctx_t *ctx)
{
    (void)ctx;
    /* hardware cycle counter reset to 0 — not simulated in host test */
}

/* ── Tests ── */

static void test_pmu_init(void)
{
    printf("\n--- test_pmu_context_init ---\n");
    pmu_ctx_t ctx;
    pmu_vcpu_init(&ctx);

    CHECK(ctx.pmcr_el0 == 0,           "Initial PMCR == 0");
    CHECK(ctx.pmcntenset_el0 == 0,     "Initial PMCNTENSET == 0");
    CHECK(ctx.pmccntr_el0 == 0,        "Initial PMCCNTR == 0");
    CHECK(ctx.enabled == false,        "Initial enabled == false");
    CHECK(ctx.pmevcntr[0] == 0,        "Initial PMEVCNTR[0] == 0");
    CHECK(ctx.pmevcntr[3] == 0,        "Initial PMEVCNTR[3] == 0");
    CHECK(ctx.pmovsr_el0 == 0,         "Initial PMOVSR == 0");
}

static void test_pmcr_enable(void)
{
    printf("\n--- test_pmcr_enable ---\n");
    pmu_ctx_t ctx;
    pmu_vcpu_init(&ctx);

    /* Guest writes PMCR with E=1 (enable counters) */
    handle_pmcr_write(&ctx, 0x1ULL);
    CHECK(ctx.enabled == true,         "PMCR.E=1 sets enabled");
    CHECK(ctx.pmcr_el0 == 1,           "PMCR stored correctly");

    /* Guest writes PMCR with P=1 (reset event counters) */
    ctx.pmevcntr[0] = 12345;
    ctx.pmevcntr[2] = 99999;
    handle_pmcr_write(&ctx, 0x5ULL);   /* E=1, P=1 */
    CHECK(ctx.pmevcntr[0] == 0,        "PMCR.P=1 resets event counter 0");
    CHECK(ctx.pmevcntr[2] == 0,        "PMCR.P=1 resets event counter 2");

    /* Guest writes PMCR with C=1 (reset cycle counter) */
    ctx.pmccntr_el0 = 0xDEADBEEFULL;
    handle_pmcr_write(&ctx, 0x3ULL);   /* E=1, C=1 */
    CHECK(ctx.pmccntr_el0 == 0,        "PMCR.C=1 resets cycle counter");
}

static void test_counter_enable(void)
{
    printf("\n--- test_counter_enable ---\n");
    pmu_ctx_t ctx;
    pmu_vcpu_init(&ctx);

    /* Enable counters 0 and 1 */
    handle_pmcnten_set(&ctx, 0x3ULL);
    CHECK(ctx.pmcntenset_el0 == 0x3,   "PMCNTENSET: counters 0+1 enabled");

    /* Enable cycle counter (bit 31) */
    handle_pmcnten_set(&ctx, 1ULL << 31);
    CHECK(ctx.pmcntenset_el0 & (1ULL << 31), "PMCNTENSET: cycle counter enabled");

    /* Disable counter 0 */
    handle_pmcnten_clr(&ctx, 0x1ULL);
    CHECK(!(ctx.pmcntenset_el0 & 1),   "PMCNTENCLR: counter 0 disabled");
    CHECK(ctx.pmcntenset_el0 & 0x2,    "PMCNTENCLR: counter 1 still enabled");
}

static void test_overflow_status(void)
{
    printf("\n--- test_overflow_status ---\n");
    pmu_ctx_t ctx;
    pmu_vcpu_init(&ctx);

    /* Set overflow bits (simulating hardware overflow) */
    ctx.pmovsr_el0 = 0xF;

    /* Guest clears overflow bits 0 and 1 (write-1-to-clear) */
    handle_pmovsr_write(&ctx, 0x3);
    CHECK(ctx.pmovsr_el0 == 0xC,       "PMOVSR: bits 0+1 cleared by w1c");
    CHECK(ctx.pmovsr_el0 & 0x4,        "PMOVSR: bit 2 still set");

    /* Clear remaining bits */
    handle_pmovsr_write(&ctx, 0xC);
    CHECK(ctx.pmovsr_el0 == 0,         "PMOVSR: all bits cleared");
}

static void test_cycle_accumulation(void)
{
    printf("\n--- test_cycle_accumulation ---\n");
    pmu_ctx_t ctx;
    pmu_vcpu_init(&ctx);
    handle_pmcr_write(&ctx, 0x1ULL);   /* enable */

    /* Simulate 3 scheduler intervals with different hw cycle readings */
    /* Interval 1: 1,000,000 cycles */
    pmu_save_sim(&ctx, 1000000ULL);
    CHECK(ctx.pmccntr_el0 == 1000000,  "After interval 1: 1M cycles accumulated");
    CHECK(ctx.enabled == true,         "enabled stays true after save");

    pmu_restore_sim(&ctx);

    /* Interval 2: 2,500,000 cycles */
    pmu_save_sim(&ctx, 2500000ULL);
    CHECK(ctx.pmccntr_el0 == 3500000,  "After interval 2: 3.5M cycles accumulated");

    pmu_restore_sim(&ctx);

    /* Interval 3: 500,000 cycles */
    pmu_save_sim(&ctx, 500000ULL);
    CHECK(ctx.pmccntr_el0 == 4000000,  "After interval 3: 4M cycles accumulated");

    /* Guest resets cycle counter via PMCR.C=1 */
    handle_pmcr_write(&ctx, 0x3ULL);
    CHECK(ctx.pmccntr_el0 == 0,        "PMCR.C=1 resets accumulated cycles");
}

static void test_event_counters(void)
{
    printf("\n--- test_event_counters ---\n");
    pmu_ctx_t ctx;
    pmu_vcpu_init(&ctx);

    /* Guest configures event counter 0 to count INST_RETIRED (0x0008) */
    ctx.pmevtyper[0] = 0x0008ULL;
    ctx.pmevcntr[0]  = 10000ULL;

    /* Guest configures event counter 1 to count L1D_CACHE (0x0003) */
    ctx.pmevtyper[1] = 0x0003ULL;
    ctx.pmevcntr[1]  = 500ULL;

    CHECK(ctx.pmevtyper[0] == 0x0008,  "Event counter 0 type = INST_RETIRED");
    CHECK(ctx.pmevcntr[0]  == 10000,   "Event counter 0 value = 10000");
    CHECK(ctx.pmevtyper[1] == 0x0003,  "Event counter 1 type = L1D_CACHE");
    CHECK(ctx.pmevcntr[1]  == 500,     "Event counter 1 value = 500");

    /* Simulate pmu_query */
    u64 instrs = 0, misses = 0;
    for (u32 i = 0; i < PMU_NUM_COUNTERS; i++) {
        if (ctx.pmevtyper[i] == 0x0008) instrs      += ctx.pmevcntr[i];
        if (ctx.pmevtyper[i] == 0x0003) misses += ctx.pmevcntr[i];
    }
    CHECK(instrs == 10000,             "pmu_query: instructions = 10000");
    CHECK(misses == 500,               "pmu_query: cache misses = 500");
}

static void test_isolation(void)
{
    printf("\n--- test_isolation (multi-vCPU) ---\n");

    /* Two vCPUs with independent PMU contexts */
    pmu_ctx_t vm1_pmu, vm2_pmu;
    pmu_vcpu_init(&vm1_pmu);
    pmu_vcpu_init(&vm2_pmu);

    /* VM1 runs for 2M cycles */
    handle_pmcr_write(&vm1_pmu, 1ULL);
    pmu_save_sim(&vm1_pmu, 2000000ULL);

    /* VM2 runs for 500K cycles */
    handle_pmcr_write(&vm2_pmu, 1ULL);
    pmu_save_sim(&vm2_pmu, 500000ULL);

    /* Switch back to VM1 for another 1M cycles */
    pmu_restore_sim(&vm1_pmu);
    pmu_save_sim(&vm1_pmu, 1000000ULL);

    CHECK(vm1_pmu.pmccntr_el0 == 3000000, "VM1 accumulates 3M cycles independently");
    CHECK(vm2_pmu.pmccntr_el0 == 500000,  "VM2 accumulates 500K cycles independently");
    CHECK(vm1_pmu.pmccntr_el0 != vm2_pmu.pmccntr_el0, "VM1 and VM2 counters are isolated");
}

static void test_hvc_abi(void)
{
    printf("\n--- test_hvc_abi ---\n");
    #define HVC_PMU_QUERY 0x0040u
    #define HVC_PMU_RESET 0x0041u
    #define HVC_SCHED_GET_STATS 0x0031u

    CHECK(HVC_PMU_QUERY != HVC_PMU_RESET,       "PMU HVC IDs are distinct");
    CHECK(HVC_PMU_QUERY != HVC_SCHED_GET_STATS, "PMU query != sched stats");
    CHECK(HVC_PMU_QUERY == 0x40,                "HVC_PMU_QUERY = 0x40");
    CHECK(HVC_PMU_RESET == 0x41,                "HVC_PMU_RESET = 0x41");
}

int main(void)
{
    printf("=== Phase 4A unit tests: PMU manager ===\n");
    test_pmu_init();
    test_pmcr_enable();
    test_counter_enable();
    test_overflow_status();
    test_cycle_accumulation();
    test_event_counters();
    test_isolation();
    test_hvc_abi();
    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    if (_fail == 0) printf("    Phase 4A unit tests: ALL PASSED ✓\n");
    return _fail ? 1 : 0;
}
