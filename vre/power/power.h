/*
 * power.h — Power and clock manager (Phase 2 §2.2.4)
 *
 * Tracks power/clock state per VM. When a VM is suspended, its assigned
 * devices are marked clock-gated so the hypervisor knows not to service
 * interrupts for them. When resumed, the state is restored.
 *
 * On QEMU there is no real clock controller; the state is tracked in
 * software only. On real SoCs (RPi4, S32G) this would write to the
 * platform clock controller registers.
 *
 * Integration point: vm_suspend() and vm_resume() call power_gate_vm()
 * and power_ungate_vm() respectively.
 */

#ifndef HYP_POWER_H
#define HYP_POWER_H

#include "../../include/types.h"
#include "../../include/error.h"

struct vm;

/* Power state for a single VM */
typedef enum {
    POWER_STATE_ON = 0,    /* VM running, all devices clocked */
    POWER_STATE_GATED,     /* VM suspended, devices clock-gated */
} power_state_t;

/*
 * power_init — initialise the power manager.
 * Called once from vm_subsys_init().
 */
err_t power_init(void);

/*
 * power_gate_vm — gate clocks for all devices assigned to vm.
 * Called when a VM is suspended.
 * Prevents interrupt storms from unhandled device IRQs.
 */
err_t power_gate_vm(struct vm *vm);

/*
 * power_ungate_vm — restore clocks for all devices assigned to vm.
 * Called when a VM is resumed.
 */
err_t power_ungate_vm(struct vm *vm);

/*
 * power_get_state — query the current power state of a VM.
 */
power_state_t power_get_state(u32 vm_id);

/*
 * power_is_device_gated — returns true if the device at mmio_base
 * belongs to a suspended VM and its clock is gated.
 * Used by the IRQ router to drop interrupts for gated devices.
 */
bool power_is_device_gated(u64 mmio_base);

#endif /* HYP_POWER_H */
