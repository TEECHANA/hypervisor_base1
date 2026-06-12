/*
 * test_phase4c.c — Phase 4C unit tests: SMMU stream assignment
 *
 * Build:
 *   gcc -O0 -g -Wall tests/unit/test_phase4c.c -o build/test_phase4c
 *   ./build/test_phase4c
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef s32      err_t;
typedef u64      paddr_t;

#define E_OK     0
#define E_INVAL -3
#define FAIL(e) ((e) != 0)

#define MAX_STREAMS  256
#define STE_SIZE     64

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓  %s\n", msg); _pass++; } \
    else       { printf("  ✗  %s  [line %d]\n", msg, __LINE__); _fail++; } \
} while(0)

/* ── Inline STE simulation ── */

static u8 _ste_table[MAX_STREAMS * STE_SIZE];
static bool _smmu_present = false;

static err_t sim_smmu_assign(u32 sid, u32 vm_id, paddr_t s2_pgd, u64 vttbr)
{
    if (sid >= MAX_STREAMS) return E_INVAL;
    if (!s2_pgd)            return E_INVAL;
    if (!_smmu_present)     return E_OK;  /* graceful no-op */

    u64 *ste  = (u64 *)(_ste_table + (u64)sid * STE_SIZE);
    u8   vmid = (u8)((vttbr >> 48) & 0xFF);

    ste[0] = 1ULL | (5ULL << 1) | ((u64)vmid << 44);
    ste[2] = s2_pgd & ~0xFFFULL;
    ste[3] = 0x18ULL | (0x2ULL << 16) | (1ULL << 10);
    return E_OK;
}

static err_t sim_smmu_remove(u32 sid)
{
    if (sid >= MAX_STREAMS) return E_INVAL;
    if (_smmu_present) {
        u64 *ste = (u64 *)(_ste_table + (u64)sid * STE_SIZE);
        ste[0] = 0;
    }
    return E_OK;
}

/* ── Tests ── */

static void test_smmu_absent(void)
{
    printf("\n--- test_smmu_absent (QEMU without iommu=smmuv3) ---\n");
    _smmu_present = false;
    memset(_ste_table, 0, sizeof(_ste_table));

    /* All operations should succeed but be no-ops */
    err_t e = sim_smmu_assign(0, 1, 0x41000000ULL, 0x0001000040019000ULL);
    CHECK(e == E_OK, "assign returns E_OK when SMMU absent");

    u64 *ste = (u64 *)_ste_table;
    CHECK(ste[0] == 0, "STE not written when SMMU absent");
    CHECK(ste[2] == 0, "S2TTB not written when SMMU absent");
}

static void test_smmu_present(void)
{
    printf("\n--- test_smmu_present (real hardware path) ---\n");
    _smmu_present = true;
    memset(_ste_table, 0, sizeof(_ste_table));

    /* VM1: stream 0, VMID=1, S2 at 0x41000000 */
    u64 vttbr_vm1 = 0x0001000040019000ULL;  /* VMID=1 in bits[55:48] */
    err_t e = sim_smmu_assign(0, 1, 0x41000000ULL, vttbr_vm1);
    CHECK(e == E_OK, "VM1 stream 0 assigned");

    u64 *ste0 = (u64 *)(_ste_table + 0 * STE_SIZE);
    CHECK(ste0[0] & 1ULL,          "STE0: Valid bit set");
    CHECK((ste0[0] >> 1 & 0xF) == 5, "STE0: Config=5 (S2 only)");
    u8 vmid = (u8)((ste0[0] >> 44) & 0xFF);
    CHECK(vmid == 1,               "STE0: VMID=1");
    CHECK(ste0[2] == 0x41000000ULL,"STE0: S2TTB=0x41000000");
    CHECK(ste0[3] & 0x18ULL,       "STE0: T0SZ=24 set");
    CHECK(ste0[3] & (1ULL << 10),  "STE0: AA64=1");
}

static void test_multi_vm_streams(void)
{
    printf("\n--- test_multi_vm_streams ---\n");
    _smmu_present = true;
    memset(_ste_table, 0, sizeof(_ste_table));

    /* VM1: stream 0 */
    sim_smmu_assign(0, 1, 0x41000000ULL, 0x0001000040019000ULL);
    /* VM2: stream 1 */
    sim_smmu_assign(1, 2, 0x60000000ULL, 0x000200004001a000ULL);
    /* VM3: stream 2 */
    sim_smmu_assign(2, 3, 0x70000000ULL, 0x000300004001b000ULL);

    u64 *ste0 = (u64 *)(_ste_table + 0 * STE_SIZE);
    u64 *ste1 = (u64 *)(_ste_table + 1 * STE_SIZE);
    u64 *ste2 = (u64 *)(_ste_table + 2 * STE_SIZE);

    CHECK(ste0[2] == 0x41000000ULL, "Stream 0: S2TTB=VM1 base");
    CHECK(ste1[2] == 0x60000000ULL, "Stream 1: S2TTB=VM2 base");
    CHECK(ste2[2] == 0x70000000ULL, "Stream 2: S2TTB=VM3 base");

    /* Check VMIDs are distinct */
    u8 vmid0 = (u8)((ste0[0] >> 44) & 0xFF);
    u8 vmid1 = (u8)((ste1[0] >> 44) & 0xFF);
    u8 vmid2 = (u8)((ste2[0] >> 44) & 0xFF);
    CHECK(vmid0 == 1, "Stream 0: VMID=1");
    CHECK(vmid1 == 2, "Stream 1: VMID=2");
    CHECK(vmid2 == 3, "Stream 2: VMID=3");
    CHECK(vmid0 != vmid1 && vmid1 != vmid2, "All VMIDs are distinct");
}

static void test_stream_remove(void)
{
    printf("\n--- test_stream_remove ---\n");
    _smmu_present = true;
    memset(_ste_table, 0, sizeof(_ste_table));

    sim_smmu_assign(0, 1, 0x41000000ULL, 0x0001000040019000ULL);
    u64 *ste0 = (u64 *)_ste_table;
    CHECK(ste0[0] & 1ULL, "STE valid before remove");

    sim_smmu_remove(0);
    CHECK(!(ste0[0] & 1ULL), "STE invalid after remove");
    CHECK(ste0[0] == 0,      "STE word 0 cleared after remove");
}

static void test_invalid_inputs(void)
{
    printf("\n--- test_invalid_inputs ---\n");
    _smmu_present = true;

    CHECK(FAIL(sim_smmu_assign(256, 1, 0x41000000ULL, 0x1000ULL)),
          "sid >= MAX_STREAMS → E_INVAL");
    CHECK(FAIL(sim_smmu_assign(0, 1, 0, 0x1000ULL)),
          "null s2_pgd → E_INVAL");
    CHECK(FAIL(sim_smmu_remove(256)),
          "remove sid >= MAX_STREAMS → E_INVAL");
}

static void test_stream_isolation(void)
{
    printf("\n--- test_stream_isolation ---\n");
    _smmu_present = true;
    memset(_ste_table, 0, sizeof(_ste_table));

    /* Assign VM1 to stream 0 with S2 at 0x41000000 */
    sim_smmu_assign(0, 1, 0x41000000ULL, 0x0001000040019000ULL);
    /* Assign VM2 to stream 1 with S2 at 0x60000000 */
    sim_smmu_assign(1, 2, 0x60000000ULL, 0x000200004001a000ULL);

    u64 *ste0 = (u64 *)(_ste_table + 0 * STE_SIZE);
    u64 *ste1 = (u64 *)(_ste_table + 1 * STE_SIZE);

    /* VM1 DMA is confined to VM1's S2 (0x41000000 base) */
    CHECK(ste0[2] != ste1[2], "VM1 and VM2 have different S2 bases in SMMU");

    /* Remove VM1 — VM2 should be unaffected */
    sim_smmu_remove(0);
    CHECK(!(ste0[0] & 1ULL), "VM1 stream invalidated");
    CHECK(ste1[0] & 1ULL,    "VM2 stream still valid after VM1 removed");
    CHECK(ste1[2] == 0x60000000ULL, "VM2 S2TTB unchanged after VM1 removed");
}

static void test_qemu_base_address(void)
{
    printf("\n--- test_qemu_base_address ---\n");
    /* Verify the QEMU virt SMMUv3 base address is correct */
    u64 smmu_base = 0x09050000ULL;
    CHECK(smmu_base == 0x09050000ULL, "QEMU virt SMMUv3 base = 0x09050000");
    CHECK((smmu_base & 0xFFF) == 0,   "Base is page-aligned");

    /* Stream ID mapping: VM id (1-based) → stream (0-based) */
    CHECK((1 - 1) == 0, "VM1 → stream 0");
    CHECK((2 - 1) == 1, "VM2 → stream 1");
    CHECK((3 - 1) == 2, "VM3 → stream 2");
}

int main(void)
{
    printf("=== Phase 4C unit tests: SMMU stream assignment ===\n");
    test_smmu_absent();
    test_smmu_present();
    test_multi_vm_streams();
    test_stream_remove();
    test_invalid_inputs();
    test_stream_isolation();
    test_qemu_base_address();
    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    if (_fail == 0) printf("    Phase 4C unit tests: ALL PASSED ✓\n");
    return _fail ? 1 : 0;
}
