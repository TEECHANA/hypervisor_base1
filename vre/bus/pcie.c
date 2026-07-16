/*
 * pcie.c — PCIe bus manager (Phase 4D §2.2.5)
 *
 * Implementation notes:
 *
 * Trap mechanism:
 *   pcie_init() calls mmio_register() to register the ECAM range
 *   (0x3F000000, 16MB) with pcie_ecam_emulate as the handler.
 *   The ECAM range must NOT be mapped in any VM's Stage-2 tables —
 *   it is intentionally left unmapped so guest accesses fault to EL2.
 *   mmio_handle() in fault_dabt() then calls pcie_ecam_emulate().
 *
 * Config space forwarding:
 *   For owned BDFs: we forward reads directly from the physical ECAM.
 *   The ECAM is mapped in the hypervisor's own EL2 address space since
 *   the hypervisor identity-maps all physical RAM and device MMIO at boot.
 *   For unowned BDFs: return 0xFFFFFFFF (standard "device not present").
 *
 * Write filtering:
 *   BAR writes: forwarded as-is (the BAR belongs to the VM's device).
 *   Command register (offset 0x04) writes: forwarded but logged.
 *   All other writes: forwarded to the physical config space.
 *
 * Default assignment:
 *   pcie_assign_vm(1, 0xFF, 0, 0) — VM1 (Linux) gets all bus 0 devices.
 *   RTOS and Android get no PCIe devices — any ECAM access returns 0xFFFF.
 */

#include "pcie.h"
#include "../../vre/device/mmio_trap.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

/* BDF assignment table */
static pcie_bdf_t _assignments[PCIE_MAX_ASSIGNMENTS];
static u32        _num_assignments = 0;

/* Physical ECAM pointer — direct EL2 access to hardware config space */
static volatile u32 *_ecam = NULL;

/* ── pcie_init ── */

err_t pcie_init(void)
{
    memset(_assignments, 0, sizeof(_assignments));
    _num_assignments = 0;

    /* Map physical ECAM for EL2 direct access (identity mapped) */
    _ecam = (volatile u32 *)(uintptr_t)PCIE_ECAM_BASE;

    /* Register ECAM range as MMIO trap — guests DABT on access */
    err_t e = mmio_register(PCIE_ECAM_BASE, PCIE_ECAM_SIZE,
                             pcie_ecam_emulate, NULL);
    if (FAIL(e)) {
        LOG_WARN("PCIe: failed to register ECAM trap (err=%d)", (int)e);
        return e;
    }

    LOG_INFO("PCIe: bus manager initialised, ECAM=%lx size=%lx",
             PCIE_ECAM_BASE, PCIE_ECAM_SIZE);
    return E_OK;
}

/* ── pcie_assign_vm ── */

err_t pcie_assign_vm(u32 vm_id, u8 bus, u8 dev, u8 fn)
{
    if (_num_assignments >= PCIE_MAX_ASSIGNMENTS) return E_INVAL;

    _assignments[_num_assignments].vm_id = vm_id;
    _assignments[_num_assignments].bus   = bus;
    _assignments[_num_assignments].dev   = dev;
    _assignments[_num_assignments].fn    = fn;
    _num_assignments++;

    if (bus == 0xFF)
        LOG_INFO("PCIe: VM%u owns all bus %u devices", vm_id, bus);
    else
        LOG_INFO("PCIe: VM%u owns BDF %02x:%02x.%x", vm_id, bus, dev, fn);

    return E_OK;
}

/* ── pcie_vm_owns_bdf ── */

bool pcie_vm_owns_bdf(u32 vm_id, u8 bus, u8 dev, u8 fn)
{
    for (u32 i = 0; i < _num_assignments; i++) {
        if (_assignments[i].vm_id != vm_id) continue;
        /* Wildcard: bus=0xFF means all devices on that bus */
        if (_assignments[i].bus == 0xFF) return true;
        if (_assignments[i].bus == bus &&
            _assignments[i].dev == dev &&
            _assignments[i].fn  == fn)
            return true;
    }
    return false;
}

/* ── pcie_ecam_emulate ── */

/*
 * Called from mmio_handle() whenever a guest accesses the ECAM range.
 * addr     : the physical address of the config access
 * is_write : true for STR/STP (config write), false for LDR (config read)
 * val      : in/out — value to write or read result
 * priv     : unused
 *
 * Returns E_OK always — unknown accesses return 0xFFFFFFFF harmlessly.
 */
err_t pcie_ecam_emulate(u64 addr, bool is_write, u64 *val, void *priv)
{
    (void)priv;

    if (addr < PCIE_ECAM_BASE || addr >= PCIE_ECAM_BASE + PCIE_ECAM_SIZE) {
        if (!is_write && val) *val = PCIE_VENDOR_INVALID;
        return E_OK;
    }

    /* Decode BDF from address */
    u8  bus, dev, fn;
    u16 reg;
    pcie_decode_addr(addr, &bus, &dev, &fn, &reg);

    /* Determine current VM from mmio ownership tracking */
    extern u32 mmio_get_current_vm(void);
    u32 cur_vm = mmio_get_current_vm();

    /* Check ownership */
    if (!pcie_vm_owns_bdf(cur_vm, bus, dev, fn)) {
        /* Not owned — return "device not present" for reads, ignore writes */
        if (!is_write && val) *val = (u64)PCIE_VENDOR_INVALID;
        return E_OK;
    }

    /* Owned — forward to physical ECAM */
    u32 ecam_off = (u32)(addr - PCIE_ECAM_BASE);
    volatile u32 *cfg = (volatile u32 *)((u8 *)_ecam + ecam_off);

    if (is_write) {
        /* Filter: log command register writes (bus mastering changes) */
        if (reg == 0x04)
            LOG_INFO("PCIe: VM%u BDF %02x:%02x.%x CMD write %lx",
                     cur_vm, bus, dev, fn, val ? *val : 0);
        if (val) *cfg = (u32)*val;
    } else {
        if (val) *val = (u64)*cfg;
    }

    return E_OK;
}
