/*
 * mmio_trap.h — MMIO emulation and device protection (Phase 2 §2.2.1)
 *
 * Phase 2 additions:
 *   mmio_register_vm_device() — register a device with VM ownership info.
 *     When fault_dabt fires for a VM that does NOT own the device, the
 *     handler logs a protection violation instead of silently skipping.
 *   mmio_set_current_vm()     — called by scheduler on every context switch
 *     so mmio_handle knows which VM is currently running.
 */

#ifndef HYP_MMIO_TRAP_H
#define HYP_MMIO_TRAP_H

#include "../../include/types.h"
#include "../../include/error.h"

/* Callback type for device emulation handlers */
typedef err_t (*mmio_fn)(u64 addr, bool is_write, u64 *val, void *priv);

/*
 * mmio_register — register an MMIO region for emulation.
 * No VM ownership — any guest can trigger this handler.
 * Used for shared devices (e.g. GIC emulation).
 */
err_t mmio_register(u64 base, u64 sz, mmio_fn fn, void *priv);

/*
 * mmio_register_vm_device — register an MMIO region owned by a specific VM.
 *
 * vm_id    : the VM that owns this device (0 = any VM allowed)
 * base/sz  : physical address range of the device
 * fn       : emulation handler (NULL = passthrough, no emulation needed)
 * priv     : opaque pointer passed to fn
 *
 * If a different VM accesses this range, mmio_handle() logs a protection
 * violation and advances ELR past the faulting instruction (device access
 * reads return 0, writes are discarded). This prevents cross-VM device
 * access without crashing the offending VM.
 */
err_t mmio_register_vm_device(u32 vm_id, u64 base, u64 sz,
                               mmio_fn fn, void *priv);

/*
 * mmio_set_current_vm — called by the scheduler on every context switch.
 * mmio_handle() uses this to check VM ownership of MMIO regions.
 */
void mmio_set_current_vm(u32 vm_id);
u32  mmio_get_current_vm(void);   /* Phase 4D */

/*
 * mmio_handle — dispatch a Stage-2 DABT fault at fault_addr.
 * Called from fault_dabt() when the faulting address is in the MMIO range.
 */
void mmio_handle(u64 fault_addr, u64 esr, void *regs);

#endif /* HYP_MMIO_TRAP_H */
