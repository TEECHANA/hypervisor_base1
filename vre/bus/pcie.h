/*
 * pcie.h — PCIe bus manager (Phase 4D §2.2.5)
 *
 * Problem this solves:
 *   QEMU virt exposes a PCIe ECAM (Enhanced Configuration Access Mechanism)
 *   region at physical address 0x3F000000 (16MB). Without a bus manager,
 *   any VM can access any PCIe device's config space directly — a VM could
 *   reprogram BARs of devices owned by another VM, or read identity
 *   information from devices it shouldn't know about.
 *
 * What Phase 4D implements:
 *
 *   1. ECAM range trapped to EL2 via Stage-2 page table (unmapped → DABT).
 *      fault_dabt → mmio_handle → pcie_ecam_emulate().
 *
 *   2. Per-VM BDF whitelist: each VM has a list of allowed BDFs.
 *      Linux (VM1) gets all devices. RTOS/Android get none by default.
 *      A BDF not on the VM's whitelist returns 0xFFFFFFFF (device not present).
 *
 *   3. Config space filter: BAR writes from a guest are validated before
 *      being forwarded to hardware. Writes to command register bits that
 *      would affect bus mastering or memory access are checked.
 *
 *   4. pcie_assign_vm(vm_id, bus, dev, fn) — assign a BDF to a VM.
 *      Called during platform init; Linux gets bus 0 devices by default.
 *
 * ECAM addressing:
 *   Physical address = ECAM_BASE + (bus << 20) | (dev << 15) | (fn << 12) | reg
 *   For QEMU virt: bus=0, so addr = 0x3F000000 + (dev<<15) + (fn<<12) + reg
 *
 * Config space header (Type 0):
 *   0x00: Vendor ID (16) | Device ID (16)
 *   0x04: Command (16)   | Status (16)
 *   0x08: Revision (8)   | Class code (24)
 *   0x0C: Cache line (8) | Latency (8) | Header type (8) | BIST (8)
 *   0x10: BAR0
 *   0x14: BAR1
 *   ...
 */

#ifndef HYP_PCIE_H
#define HYP_PCIE_H

#include "../../include/types.h"
#include "../../include/error.h"

/* QEMU virt PCIe ECAM base and size */
#define PCIE_ECAM_BASE   0x3F000000ULL
#define PCIE_ECAM_SIZE   0x01000000ULL   /* 16 MB */

/* Maximum BDF entries per VM */
#define PCIE_MAX_BDF_PER_VM   16
/* Maximum total BDF assignments across all VMs */
#define PCIE_MAX_ASSIGNMENTS  64

/* Returned for config reads to unassigned/unknown devices */
#define PCIE_VENDOR_INVALID   0xFFFFFFFFu

/*
 * pcie_bdf_t — a Bus/Device/Function tuple identifying one PCIe endpoint.
 */
typedef struct {
    u8  bus;
    u8  dev;
    u8  fn;
    u32 vm_id;    /* owning VM (0 = unassigned) */
} pcie_bdf_t;

/* ── API ── */

/*
 * pcie_init — initialise the PCIe bus manager.
 * Registers the ECAM range as an MMIO trap via mmio_register().
 * Returns E_OK always — no hardware probing needed (trap-based).
 */
err_t pcie_init(void);

/*
 * pcie_assign_vm — assign a PCIe device (BDF) to a VM.
 * After this call, that VM's guests will see the real device config.
 * Other VMs reading the same BDF get 0xFFFFFFFF (not present).
 *
 * bus=0xff means "assign all devices on this bus to vm_id" (wildcard).
 */
err_t pcie_assign_vm(u32 vm_id, u8 bus, u8 dev, u8 fn);

/*
 * pcie_ecam_emulate — MMIO handler called from mmio_handle().
 * Intercepts guest ECAM reads/writes, checks BDF ownership,
 * and either forwards to hardware or returns 0xFFFFFFFF.
 */
err_t pcie_ecam_emulate(u64 addr, bool is_write, u64 *val, void *priv);

/*
 * pcie_vm_owns_bdf — return true if vm_id is allowed to access this BDF.
 * Used by pcie_ecam_emulate for access control.
 */
bool pcie_vm_owns_bdf(u32 vm_id, u8 bus, u8 dev, u8 fn);

/*
 * pcie_decode_addr — extract BDF and register offset from an ECAM address.
 */
static inline void pcie_decode_addr(u64 addr, u8 *bus, u8 *dev, u8 *fn, u16 *reg)
{
    u64 off = addr - PCIE_ECAM_BASE;
    *bus = (u8)((off >> 20) & 0xFF);
    *dev = (u8)((off >> 15) & 0x1F);
    *fn  = (u8)((off >> 12) & 0x07);
    *reg = (u16)(off & 0xFFF);
}

#endif /* HYP_PCIE_H */
