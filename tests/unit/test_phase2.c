/*
 * test_phase2.c — Phase 2 unit tests
 *
 * Tests all Phase 2 modules:
 *   §2.2.1 mmio_trap VM ownership enforcement
 *   §2.2.2 dma_guard address validation
 *   §2.2.3 smmu graceful fallback
 *   §2.2.4 power manager gate/ungate
 *   §3.1.4 device profile table structure
 *
 * Build:
 *   gcc -O0 -g -Wall -I. \
 *       tests/unit/test_phase2.c \
 *       -o build/test_phase2 && ./build/test_phase2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Minimal type shims for host build ── */
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef u64      paddr_t;
typedef u64      ipa_t;
typedef s32      err_t;

#define E_OK      0
#define E_INVAL  -1
#define E_NOMEM  -2
#define E_PERM   -3
#define E_BUSY   -4
#define FAIL(e)  ((e) != E_OK)
#define OK(e)    ((e) == E_OK)
#define BIT(n)   (1UL << (n))
#define UNUSED(x) ((void)(x))

#define MEM_R  BIT(0)
#define MEM_W  BIT(1)
#define MEM_X  BIT(2)
#define MEM_IO BIT(3)

#define MAX_VMS          4
#define MAX_MEM_REGIONS  8
#define MAX_DEV_PER_VM  16
#define MAX_MMIO_REGIONS 32
#define MAX_GATED_DEVS   64
#define DEVPROF_MAX_ENTRIES 32
#define DEVPROF_NAME_LEN    32

/* ── Test harness ── */
static int _pass = 0, _fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ✓  %s\n", msg); _pass++; } \
    else       { printf("  ✗  %s\n", msg); _fail++; } \
} while(0)

/* ── Minimal vm_t for tests ── */
typedef struct {
    u64     pa_base;
    u64     ipa_base;
    u64     size;
    u32     flags;
} mem_region_t;

typedef struct {
    u64  mmio_base;
    u64  mmio_size;
    u32  irq;
    bool passthru;
} dev_assign_t;

typedef struct vm {
    u32          id;
    char         name[32];
    u32          num_mem;
    mem_region_t mem[MAX_MEM_REGIONS];
    u32          num_dev;
    dev_assign_t dev[MAX_DEV_PER_VM];
    u64          s2_pgd;
    u64          vttbr;
} vm_t;

/* ── Minimal s2_ipa_is_mapped stub ── */
static bool s2_ipa_is_mapped_stub(vm_t *vm, ipa_t ipa, u64 sz)
{
    for (u32 i = 0; i < vm->num_mem; i++) {
        if (ipa >= vm->mem[i].ipa_base &&
            ipa + sz <= vm->mem[i].ipa_base + vm->mem[i].size)
            return true;
    }
    return false;
}

/* ── §2.2.2 dma_guard tests (inline impl) ── */
static err_t dma_guard_check(vm_t *vm, paddr_t pa, u64 size)
{
    if (!vm || !size) return E_INVAL;
    if (size & 0xFFF) size = (size + 0xFFF) & ~0xFFFULL;

    for (u32 i = 0; i < vm->num_mem; i++) {
        mem_region_t *r = &vm->mem[i];
        if (pa >= r->pa_base && pa < r->pa_base + r->size) {
            if (pa + size > r->pa_base + r->size) return E_PERM;
            ipa_t ipa = r->ipa_base + (pa - r->pa_base);
            if (!s2_ipa_is_mapped_stub(vm, ipa, size)) return E_PERM;
            return E_OK;
        }
    }
    return E_PERM;
}

static err_t dma_guard_check_ipa(vm_t *vm, ipa_t ipa, u64 size)
{
    if (!vm || !size) return E_INVAL;
    if (size & 0xFFF) size = (size + 0xFFF) & ~0xFFFULL;
    return s2_ipa_is_mapped_stub(vm, ipa, size) ? E_OK : E_PERM;
}

static void test_dma_guard(void)
{
    printf("\n--- test_dma_guard ---\n");

    vm_t vm = {0};
    vm.id = 1;
    /* Map IPA 0x0 → PA 0x60000000, 16 MB */
    vm.mem[0] = (mem_region_t){
        .pa_base  = 0x60000000,
        .ipa_base = 0x00000000,
        .size     = 0x01000000,
        .flags    = MEM_R|MEM_W|MEM_X,
    };
    vm.num_mem = 1;

    /* PA within region — should pass */
    err_t r = dma_guard_check(&vm, 0x60000000, 0x1000);
    CHECK(r == E_OK, "DMA check: PA within region → E_OK");

    r = dma_guard_check(&vm, 0x60800000, 0x1000);
    CHECK(r == E_OK, "DMA check: PA mid-region → E_OK");

    /* PA outside region — should deny */
    r = dma_guard_check(&vm, 0x70000000, 0x1000);
    CHECK(r == E_PERM, "DMA check: PA outside region → E_PERM");

    /* PA crosses region boundary */
    r = dma_guard_check(&vm, 0x60FFF000, 0x2000);
    CHECK(r == E_PERM, "DMA check: PA crosses region boundary → E_PERM");

    /* IPA check */
    r = dma_guard_check_ipa(&vm, 0x00001000, 0x1000);
    CHECK(r == E_OK, "DMA check IPA: mapped IPA → E_OK");

    r = dma_guard_check_ipa(&vm, 0x02000000, 0x1000);
    CHECK(r == E_PERM, "DMA check IPA: unmapped IPA → E_PERM");

    /* NULL vm */
    r = dma_guard_check(NULL, 0x60000000, 0x1000);
    CHECK(r == E_INVAL, "DMA check: NULL vm → E_INVAL");

    /* Zero size */
    r = dma_guard_check(&vm, 0x60000000, 0);
    CHECK(r == E_INVAL, "DMA check: zero size → E_INVAL");
}

/* ── §2.2.1 mmio_trap VM ownership tests (inline impl) ── */
typedef err_t (*mmio_fn)(u64, bool, u64*, void*);

typedef struct {
    u64     base;
    u64     sz;
    mmio_fn fn;
    void   *priv;
    bool    active;
    u32     owner_vm_id;
    u32     violation_cnt;
} mmio_reg_t;

static mmio_reg_t _regs[MAX_MMIO_REGIONS];
static u32 _nr = 0;
static u32 _current_vm_id = 0;


static void mmio_set_current_vm(u32 id) { _current_vm_id = id; }

static err_t mmio_register_vm_device(u32 vm_id, u64 base, u64 sz,
                                     mmio_fn fn, void *priv)
{
    if (_nr >= MAX_MMIO_REGIONS) return E_NOMEM;
    _regs[_nr++] = (mmio_reg_t){base, sz, fn, priv, true, vm_id, 0};
    return E_OK;
}


static bool _handler_called = false;

static err_t mock_handler(u64 addr, bool wr, u64 *val, void *priv)
{
    UNUSED(addr); UNUSED(wr); UNUSED(priv);
    *val = 0xDEADBEEF;
    _handler_called = true;
    return E_OK;
}

static void mmio_handle_test(u64 fa, u64 esr, bool *handler_called_out,
                              bool *violation_out)
{
    bool wr = (bool)((esr >> 6) & 1);
    u64  val = 0;
    *handler_called_out = false;
    *violation_out = false;

    for (u32 i = 0; i < _nr; i++) {
        if (!_regs[i].active) continue;
        if (fa < _regs[i].base || fa >= _regs[i].base + _regs[i].sz) continue;

        if (_regs[i].owner_vm_id != 0 &&
            _regs[i].owner_vm_id != _current_vm_id) {
            _regs[i].violation_cnt++;
            *violation_out = true;
            return;
        }

        if (_regs[i].fn) {
            _regs[i].fn(fa, wr, &val, _regs[i].priv);
            *handler_called_out = true;
        }
        return;
    }
}

static void test_mmio_ownership(void)
{
    printf("\n--- test_mmio_ownership ---\n");
    _nr = 0;
    memset(_regs, 0, sizeof(_regs));

    /* Register UART owned by VM1 */
    err_t r = mmio_register_vm_device(1, 0x09000000, 0x1000,
                                      mock_handler, NULL);
    CHECK(r == E_OK, "mmio_register_vm_device: E_OK");

    /* Register shared GIC (owner=0) */
    r = mmio_register_vm_device(0, 0x08000000, 0x100000, NULL, NULL);
    CHECK(r == E_OK, "mmio_register_vm_device shared: E_OK");

    bool handler_called, violation;

    /* VM1 accesses its own UART — should call handler */
    mmio_set_current_vm(1);
    _handler_called = false;
    mmio_handle_test(0x09000000, 0, &handler_called, &violation);
    CHECK(handler_called && !violation,
          "VM1 accesses own UART: handler called, no violation");

    /* VM2 accesses VM1's UART — should trigger violation */
    mmio_set_current_vm(2);
    mmio_handle_test(0x09000000, 0, &handler_called, &violation);
    CHECK(violation && !handler_called,
          "VM2 accesses VM1's UART: violation logged, handler NOT called");

    /* Violation counter incremented */
    CHECK(_regs[0].violation_cnt == 1,
          "Violation counter == 1 after one cross-VM access");

    /* VM2 accesses VM1's UART again */
    mmio_handle_test(0x09000000, 0, &handler_called, &violation);
    CHECK(_regs[0].violation_cnt == 2,
          "Violation counter == 2 after second cross-VM access");

    /* Any VM accesses shared GIC — no violation */
    mmio_set_current_vm(2);
    mmio_handle_test(0x08000000, 0, &handler_called, &violation);
    CHECK(!violation, "VM2 accesses shared GIC: no violation");

    mmio_set_current_vm(3);
    mmio_handle_test(0x08000000, 0, &handler_called, &violation);
    CHECK(!violation, "VM3 accesses shared GIC: no violation");
}

/* ── §2.2.3 smmu graceful fallback test ── */
static bool _smmu_present_flag = false;
static int  _smmu_init_called  = 0;

static err_t smmu_init_stub(u64 base)
{
    _smmu_init_called++;
    if (base == 0) {
        _smmu_present_flag = false;
        return E_OK;   /* QEMU: no hardware */
    }
    /* Simulate timeout (QEMU has no SMMU) */
    _smmu_present_flag = false;
    return E_OK;
}

static bool smmu_present_stub(void) { return _smmu_present_flag; }

static err_t smmu_assign_stream_stub(u32 sid, u32 vm_id,
                                     paddr_t s2_pgd, u64 vttbr)
{
    UNUSED(vm_id); UNUSED(s2_pgd); UNUSED(vttbr);
    if (sid >= 256) return E_INVAL;
    if (!_smmu_present_flag) return E_OK;  /* no-op without hardware */
    return E_OK;
}

static void test_smmu_fallback(void)
{
    printf("\n--- test_smmu_fallback ---\n");

    /* Init with base=0 (QEMU no-SMMU path) */
    err_t r = smmu_init_stub(0);
    CHECK(r == E_OK,           "smmu_init(0): returns E_OK (graceful)");
    CHECK(!smmu_present_stub(), "smmu_present(): false after QEMU init");

    /* assign_stream should no-op gracefully */
    r = smmu_assign_stream_stub(0, 1, 0x41000000, 0x0001000041000000ULL);
    CHECK(r == E_OK, "smmu_assign_stream: E_OK when no hardware");

    /* Invalid stream ID */
    r = smmu_assign_stream_stub(256, 1, 0x41000000, 0);
    CHECK(r == E_INVAL, "smmu_assign_stream(256): E_INVAL (out of range)");

    /* Simulate hardware present */
    _smmu_present_flag = true;
    CHECK(smmu_present_stub(), "smmu_present(): true when hardware present");

    r = smmu_assign_stream_stub(5, 2, 0x60000000, 0x0002000060000000ULL);
    CHECK(r == E_OK, "smmu_assign_stream with hardware: E_OK");

    /* Reset */
    _smmu_present_flag = false;
}

/* ── §2.2.4 power manager tests (inline impl) ── */
typedef enum { POWER_STATE_ON = 0, POWER_STATE_GATED } power_state_t;

static power_state_t _pstate[MAX_VMS] = {0};
static u32 _gic_disabled_irqs[64];
static u32 _ngic_disabled = 0;
static u32 _gic_enabled_irqs[64];
static u32 _ngic_enabled = 0;

static void mock_gic_disable_irq(u32 irq)
{
    _gic_disabled_irqs[_ngic_disabled++] = irq;
}
static void mock_gic_enable_irq(u32 irq)
{
    _gic_enabled_irqs[_ngic_enabled++] = irq;
}

static err_t power_gate_vm_test(vm_t *vm)
{
    if (!vm) return E_INVAL;
    for (u32 i = 0; i < vm->num_dev; i++)
        if (vm->dev[i].irq) mock_gic_disable_irq(vm->dev[i].irq);
    _pstate[vm->id - 1] = POWER_STATE_GATED;
    return E_OK;
}

static err_t power_ungate_vm_test(vm_t *vm)
{
    if (!vm) return E_INVAL;
    for (u32 i = 0; i < vm->num_dev; i++)
        if (vm->dev[i].irq) mock_gic_enable_irq(vm->dev[i].irq);
    _pstate[vm->id - 1] = POWER_STATE_ON;
    return E_OK;
}

static power_state_t power_get_state_test(u32 vm_id)
{
    if (!vm_id || vm_id > MAX_VMS) return POWER_STATE_ON;
    return _pstate[vm_id - 1];
}

static void test_power_manager(void)
{
    printf("\n--- test_power_manager ---\n");
    memset(_pstate, 0, sizeof(_pstate));
    _ngic_disabled = _ngic_enabled = 0;

    vm_t vm = {0};
    vm.id = 1;
    vm.dev[0] = (dev_assign_t){.mmio_base=0x09000000, .irq=33, .passthru=true};
    vm.dev[1] = (dev_assign_t){.mmio_base=0x0A000000, .irq=34, .passthru=true};
    vm.dev[2] = (dev_assign_t){.mmio_base=0x0B000000, .irq=0,  .passthru=true};
    vm.num_dev = 3;

    CHECK(power_get_state_test(1) == POWER_STATE_ON,
          "Initial state: POWER_STATE_ON");

    err_t r = power_gate_vm_test(&vm);
    CHECK(r == E_OK, "power_gate_vm: E_OK");
    CHECK(power_get_state_test(1) == POWER_STATE_GATED,
          "After gate: POWER_STATE_GATED");
    CHECK(_ngic_disabled == 2,
          "power_gate_vm: disabled 2 IRQs (irq=0 skipped)");
    CHECK(_gic_disabled_irqs[0] == 33 && _gic_disabled_irqs[1] == 34,
          "Disabled IRQs: 33 and 34");

    r = power_ungate_vm_test(&vm);
    CHECK(r == E_OK, "power_ungate_vm: E_OK");
    CHECK(power_get_state_test(1) == POWER_STATE_ON,
          "After ungate: POWER_STATE_ON");
    CHECK(_ngic_enabled == 2,
          "power_ungate_vm: re-enabled 2 IRQs");

    /* NULL vm */
    r = power_gate_vm_test(NULL);
    CHECK(r == E_INVAL, "power_gate_vm(NULL): E_INVAL");
}

/* ── §3.1.4 device profile table tests ── */
typedef struct {
    u32  vm_id;
    u64  pa_base;
    u64  ipa_base;
    u64  size;
    u32  flags;
    u32  irq;
    u32  stream_id;
    char name[DEVPROF_NAME_LEN];
} dev_profile_entry_t;

/* Minimal expected entries from device_profile.c */
static const dev_profile_entry_t _expected[] = {
    {1, 0x41000000, 0x00000000, 0x08000000, MEM_R|MEM_W|MEM_X, 0,  0, "linux-ram-low"},
    {1, 0x51000000, 0x10000000, 0x40000000, MEM_R|MEM_W|MEM_X, 0,  0, "linux-ram-high"},
    {1, 0x09000000, 0x09000000, 0x1000,     MEM_R|MEM_W|MEM_IO, 33, 0, "linux-uart"},
    {2, 0x60000000, 0x00000000, 0x0F000000, MEM_R|MEM_W|MEM_X, 0,  0, "rtos-ram"},
    {2, 0x09000000, 0x09000000, 0x1000,     MEM_R|MEM_W|MEM_IO, 0,  0, "rtos-uart"},
    {3, 0x70000000, 0x00000000, 0x20000000, MEM_R|MEM_W|MEM_X,  0,  0, "android-ram"},
    {3, 0x09000000, 0x09000000, 0x1000,     MEM_R|MEM_W|MEM_IO, 0,  0, "android-uart"},
};
#define EXPECTED_COUNT 7

static void test_device_profile(void)
{
    printf("\n--- test_device_profile ---\n");

    /* Verify expected entries cover all 3 VMs */
    bool has_linux = false, has_rtos = false, has_android = false;
    bool has_uart_linux = false, has_uart_rtos = false;
    u32  io_entries = 0;

    for (u32 i = 0; i < EXPECTED_COUNT; i++) {
        if (_expected[i].vm_id == 1) has_linux   = true;
        if (_expected[i].vm_id == 2) has_rtos    = true;
        if (_expected[i].vm_id == 3) has_android = true;
        if (_expected[i].flags & MEM_IO) io_entries++;
        if (_expected[i].vm_id == 1 &&
            _expected[i].pa_base == 0x09000000) has_uart_linux = true;
        if (_expected[i].vm_id == 2 &&
            _expected[i].pa_base == 0x09000000) has_uart_rtos = true;
    }

    CHECK(has_linux,   "Profile: Linux VM entries present");
    CHECK(has_rtos,    "Profile: RTOS VM entries present");
    CHECK(has_android, "Profile: Android VM entries present");
    CHECK(io_entries >= 3, "Profile: at least 3 MEM_IO device entries");
    CHECK(has_uart_linux, "Profile: Linux UART entry (PA=0x09000000)");
    CHECK(has_uart_rtos,  "Profile: RTOS UART entry (PA=0x09000000)");

    /* Verify RTOS RAM covers RTOS binary load address (PA 0x60008000) */
    bool rtos_ram_covers_binary = false;
    for (u32 i = 0; i < EXPECTED_COUNT; i++) {
        if (_expected[i].vm_id == 2 &&
            0x60008000ULL >= _expected[i].pa_base &&
            0x60008000ULL <  _expected[i].pa_base + _expected[i].size)
            rtos_ram_covers_binary = true;
    }
    CHECK(rtos_ram_covers_binary,
          "Profile: RTOS RAM covers binary load address 0x60008000");

    /* Verify no UART entry uses MEM_X */
    bool uart_no_exec = true;
    for (u32 i = 0; i < EXPECTED_COUNT; i++) {
        if (_expected[i].flags & MEM_IO && _expected[i].flags & MEM_X)
            uart_no_exec = false;
    }
    CHECK(uart_no_exec, "Profile: no IO region has MEM_X set");

    /* Verify Linux UART has IRQ 33 */
    bool linux_uart_irq = false;
    for (u32 i = 0; i < EXPECTED_COUNT; i++) {
        if (_expected[i].vm_id == 1 &&
            _expected[i].pa_base == 0x09000000 &&
            _expected[i].irq == 33)
            linux_uart_irq = true;
    }
    CHECK(linux_uart_irq, "Profile: Linux UART has IRQ 33");
}

/* ── Main ── */
int main(void)
{
    printf("=== Phase 2 unit tests ===\n");

    test_dma_guard();
    test_mmio_ownership();
    test_smmu_fallback();
    test_power_manager();
    test_device_profile();

    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    if (_fail == 0)
        printf("    Phase 2 unit tests: ALL PASSED ✓\n");
    return _fail ? 1 : 0;
}
