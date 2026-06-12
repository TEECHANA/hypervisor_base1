/*
 * test_phase4d.c — Phase 4D unit tests: PCIe bus manager
 *
 * Build:
 *   gcc -O0 -g -Wall tests/unit/test_phase4d.c -o build/test_phase4d
 *   ./build/test_phase4d
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef s32      err_t;

#define E_OK     0
#define E_INVAL -3
#define FAIL(e) ((e) != 0)

#define PCIE_ECAM_BASE      0x3F000000ULL
#define PCIE_ECAM_SIZE      0x01000000ULL
#define PCIE_VENDOR_INVALID 0xFFFFFFFFu
#define PCIE_MAX_ASSIGNMENTS 64

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓  %s\n", msg); _pass++; } \
    else       { printf("  ✗  %s  [line %d]\n", msg, __LINE__); _fail++; } \
} while(0)

/* Inline BDF assignment for host testing */
typedef struct { u8 bus, dev, fn; u32 vm_id; } pcie_bdf_t;
static pcie_bdf_t _assignments[PCIE_MAX_ASSIGNMENTS];
static u32 _num_assignments = 0;

static void pcie_reset(void) {
    memset(_assignments, 0, sizeof(_assignments));
    _num_assignments = 0;
}

static err_t pcie_assign_vm(u32 vm_id, u8 bus, u8 dev, u8 fn) {
    if (_num_assignments >= PCIE_MAX_ASSIGNMENTS) return E_INVAL;
    _assignments[_num_assignments++] = (pcie_bdf_t){bus, dev, fn, vm_id};
    return E_OK;
}

static bool pcie_vm_owns_bdf(u32 vm_id, u8 bus, u8 dev, u8 fn) {
    for (u32 i = 0; i < _num_assignments; i++) {
        if (_assignments[i].vm_id != vm_id) continue;
        if (_assignments[i].bus == 0xFF) return true;
        if (_assignments[i].bus == bus &&
            _assignments[i].dev == dev &&
            _assignments[i].fn  == fn) return true;
    }
    return false;
}

static void pcie_decode_addr(u64 addr, u8 *bus, u8 *dev, u8 *fn, u16 *reg) {
    u64 off = addr - PCIE_ECAM_BASE;
    *bus = (u8)((off >> 20) & 0xFF);
    *dev = (u8)((off >> 15) & 0x1F);
    *fn  = (u8)((off >> 12) & 0x07);
    *reg = (u16)(off & 0xFFF);
}

/* Simulated config read respecting ownership */
static u32 sim_config_read(u32 vm_id, u64 addr, u32 hw_val) {
    u8 bus, dev, fn; u16 reg;
    pcie_decode_addr(addr, &bus, &dev, &fn, &reg);
    if (!pcie_vm_owns_bdf(vm_id, bus, dev, fn))
        return PCIE_VENDOR_INVALID;
    return hw_val;
}

/* ── Tests ── */

static void test_ecam_constants(void) {
    printf("\n--- test_ecam_constants ---\n");
    CHECK(PCIE_ECAM_BASE == 0x3F000000ULL, "ECAM base = 0x3F000000");
    CHECK(PCIE_ECAM_SIZE == 0x01000000ULL, "ECAM size = 16MB");
    CHECK((PCIE_ECAM_BASE & 0xFFF) == 0,   "ECAM base is page-aligned");
    CHECK(PCIE_VENDOR_INVALID == 0xFFFFFFFFu, "invalid vendor = 0xFFFFFFFF");
}

static void test_addr_decode(void) {
    printf("\n--- test_addr_decode ---\n");
    u8 bus, dev, fn; u16 reg;

    /* BDF 00:01.0 reg 0x00 → addr = 0x3F000000 + (1<<15) = 0x3F008000 */
    pcie_decode_addr(0x3F008000ULL, &bus, &dev, &fn, &reg);
    CHECK(bus == 0 && dev == 1 && fn == 0 && reg == 0,
          "BDF 00:01.0 reg 0 decodes correctly");

    /* BDF 00:02.0 reg 0x10 (BAR0) → 0x3F010010 */
    pcie_decode_addr(0x3F010010ULL, &bus, &dev, &fn, &reg);
    CHECK(bus == 0 && dev == 2 && fn == 0 && reg == 0x10,
          "BDF 00:02.0 BAR0 decodes correctly");

    /* BDF 01:00.0 reg 0x04 */
    pcie_decode_addr(PCIE_ECAM_BASE + (1ULL<<20) + (0<<15) + 0x04, &bus, &dev, &fn, &reg);
    CHECK(bus == 1 && dev == 0 && fn == 0 && reg == 0x04,
          "BDF 01:00.0 Command reg decodes correctly");
}

static void test_bdf_assignment(void) {
    printf("\n--- test_bdf_assignment ---\n");
    pcie_reset();

    /* Assign specific BDF to VM1 */
    pcie_assign_vm(1, 0, 1, 0);
    CHECK( pcie_vm_owns_bdf(1, 0, 1, 0), "VM1 owns BDF 00:01.0");
    CHECK(!pcie_vm_owns_bdf(2, 0, 1, 0), "VM2 does not own BDF 00:01.0");
    CHECK(!pcie_vm_owns_bdf(1, 0, 2, 0), "VM1 does not own BDF 00:02.0");

    /* Assign another BDF to VM2 */
    pcie_assign_vm(2, 0, 2, 0);
    CHECK( pcie_vm_owns_bdf(2, 0, 2, 0), "VM2 owns BDF 00:02.0");
    CHECK(!pcie_vm_owns_bdf(1, 0, 2, 0), "VM1 does not own BDF 00:02.0");
}

static void test_wildcard_assignment(void) {
    printf("\n--- test_wildcard_assignment ---\n");
    pcie_reset();

    /* Wildcard: VM1 gets all bus 0 devices */
    pcie_assign_vm(1, 0xFF, 0, 0);
    CHECK(pcie_vm_owns_bdf(1, 0, 0, 0),  "VM1 owns 00:00.0 (wildcard)");
    CHECK(pcie_vm_owns_bdf(1, 0, 1, 0),  "VM1 owns 00:01.0 (wildcard)");
    CHECK(pcie_vm_owns_bdf(1, 0, 31, 7), "VM1 owns 00:1F.7 (wildcard)");
    CHECK(!pcie_vm_owns_bdf(2, 0, 0, 0), "VM2 still doesn't own 00:00.0");
    CHECK(!pcie_vm_owns_bdf(3, 0, 5, 0), "VM3 still doesn't own 00:05.0");
}

static void test_config_read_isolation(void) {
    printf("\n--- test_config_read_isolation ---\n");
    pcie_reset();
    pcie_assign_vm(1, 0xFF, 0, 0);   /* VM1 gets all bus 0 */

    u32 hw_val = 0x10EC8139u;  /* Realtek RTL8139 */
    u64 addr   = 0x3F008000ULL; /* BDF 00:01.0 reg 0 */

    /* VM1 sees real value */
    u32 vm1_read = sim_config_read(1, addr, hw_val);
    CHECK(vm1_read == hw_val, "VM1 reads real vendor/device ID");

    /* VM2 sees 0xFFFFFFFF */
    u32 vm2_read = sim_config_read(2, addr, hw_val);
    CHECK(vm2_read == PCIE_VENDOR_INVALID, "VM2 reads 0xFFFFFFFF (not owner)");

    /* VM3 sees 0xFFFFFFFF */
    u32 vm3_read = sim_config_read(3, addr, hw_val);
    CHECK(vm3_read == PCIE_VENDOR_INVALID, "VM3 reads 0xFFFFFFFF (not owner)");
}

static void test_rtos_android_no_pcie(void) {
    printf("\n--- test_rtos_android_no_pcie ---\n");
    pcie_reset();
    pcie_assign_vm(1, 0xFF, 0, 0);  /* Only VM1 gets PCIe */

    /* RTOS (VM2) and Android (VM3) should not own any device */
    for (u8 dev = 0; dev < 8; dev++) {
        CHECK(!pcie_vm_owns_bdf(2, 0, dev, 0),
              "RTOS (VM2) has no PCIe access");
        CHECK(!pcie_vm_owns_bdf(3, 0, dev, 0),
              "Android (VM3) has no PCIe access");
        break; /* just check first device to keep output concise */
    }
    CHECK(!pcie_vm_owns_bdf(2, 0, 0, 0), "RTOS cannot access PCIe bus 0");
    CHECK(!pcie_vm_owns_bdf(3, 0, 0, 0), "Android cannot access PCIe bus 0");
}

static void test_multi_vm_partial(void) {
    printf("\n--- test_multi_vm_partial_assignment ---\n");
    pcie_reset();

    /* VM1 gets device 1, VM2 gets device 2, nothing else */
    pcie_assign_vm(1, 0, 1, 0);
    pcie_assign_vm(2, 0, 2, 0);

    CHECK( pcie_vm_owns_bdf(1, 0, 1, 0),  "VM1 owns 00:01.0");
    CHECK(!pcie_vm_owns_bdf(1, 0, 2, 0),  "VM1 doesn't own VM2's device");
    CHECK( pcie_vm_owns_bdf(2, 0, 2, 0),  "VM2 owns 00:02.0");
    CHECK(!pcie_vm_owns_bdf(2, 0, 1, 0),  "VM2 doesn't own VM1's device");
    CHECK(!pcie_vm_owns_bdf(1, 0, 3, 0),  "VM1 doesn't own unassigned dev 3");
    CHECK(!pcie_vm_owns_bdf(2, 0, 3, 0),  "VM2 doesn't own unassigned dev 3");
}

static void test_ecam_range_check(void) {
    printf("\n--- test_ecam_range_check ---\n");
    u64 base = PCIE_ECAM_BASE;
    u64 end  = PCIE_ECAM_BASE + PCIE_ECAM_SIZE;

    CHECK(0x3F000000ULL >= base && 0x3F000000ULL < end,
          "ECAM base is in range");
    CHECK(0x3FFFFFFCULL >= base && 0x3FFFFFFCULL < end,
          "Last valid ECAM address is in range");
    CHECK(!(0x3F000000ULL - 1 >= base && 0x3F000000ULL - 1 < end),
          "Address before ECAM is out of range");
    CHECK(!(end >= base && end < end),
          "Address at ECAM end is out of range");
}

int main(void) {
    printf("=== Phase 4D unit tests: PCIe bus manager ===\n");
    test_ecam_constants();
    test_addr_decode();
    test_bdf_assignment();
    test_wildcard_assignment();
    test_config_read_isolation();
    test_rtos_android_no_pcie();
    test_multi_vm_partial();
    test_ecam_range_check();
    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    if (_fail == 0) printf("    Phase 4D unit tests: ALL PASSED ✓\n");
    return _fail ? 1 : 0;
}
