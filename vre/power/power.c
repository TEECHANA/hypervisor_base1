/*
 * power.c — Power and clock manager (Phase 2 §2.2.4)
 *
 * Tracks per-VM power state. When a VM is suspended:
 *   1. All device IRQs assigned to that VM are disabled in the GIC.
 *   2. The VM's power state is set to POWER_STATE_GATED.
 *   3. IRQ router checks power_is_device_gated() before injecting vIRQs.
 *
 * When a VM is resumed:
 *   1. Device IRQs are re-enabled.
 *   2. Power state set to POWER_STATE_ON.
 *
 * On real hardware (RPi4, S32G), power_gate_vm() would also write to
 * the clock controller to physically stop the device clocks. On QEMU
 * we do software-only gating via GIC disable.
 */

#include "power.h"
#include "../../core/vm/vm.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../include/config.h"

/* Per-VM power state table */
static power_state_t _state[MAX_VMS];

/* Per-device gating state: track which mmio_base addresses are gated */
#define MAX_GATED_DEVS 64
static struct {
    u64  mmio_base;
    bool gated;
} _gated_devs[MAX_GATED_DEVS];
static u32 _ngated = 0;

err_t power_init(void)
{
    memset(_state,      0, sizeof(_state));
    memset(_gated_devs, 0, sizeof(_gated_devs));
    _ngated = 0;
    LOG_INFO("Power manager: initialised (max %d VMs)", MAX_VMS);
    return E_OK;
}

err_t power_gate_vm(struct vm *vm)
{
    if (!vm) return E_INVAL;
    if (vm->id == 0 || vm->id > MAX_VMS) return E_INVAL;

    LOG_INFO("Power: gating VM%u '%s' (%u devices)",
             vm->id, vm->name, vm->num_dev);

    /* Disable GIC SPIs for all assigned devices */
    extern void gic_disable_irq(u32);
    for (u32 i = 0; i < vm->num_dev; i++) {
        if (vm->dev[i].irq) {
            gic_disable_irq(vm->dev[i].irq);
            LOG_DEBUG("Power: gated IRQ %u (VM%u device 0x%lx)",
                      vm->dev[i].irq, vm->id, vm->dev[i].mmio_base);
        }

        /* Mark this device's mmio_base as gated */
        if (_ngated < MAX_GATED_DEVS) {
            _gated_devs[_ngated].mmio_base = vm->dev[i].mmio_base;
            _gated_devs[_ngated].gated     = true;
            _ngated++;
        }
    }

    _state[vm->id - 1] = POWER_STATE_GATED;
    LOG_INFO("Power: VM%u gated", vm->id);
    return E_OK;
}

err_t power_ungate_vm(struct vm *vm)
{
    if (!vm) return E_INVAL;
    if (vm->id == 0 || vm->id > MAX_VMS) return E_INVAL;

    LOG_INFO("Power: ungating VM%u '%s'", vm->id, vm->name);

    /* Re-enable GIC SPIs */
    extern void gic_enable_irq(u32);
    for (u32 i = 0; i < vm->num_dev; i++) {
        if (vm->dev[i].irq) {
            gic_enable_irq(vm->dev[i].irq);
            LOG_DEBUG("Power: ungated IRQ %u (VM%u device 0x%lx)",
                      vm->dev[i].irq, vm->id, vm->dev[i].mmio_base);
        }

        /* Clear gated state for this device */
        for (u32 j = 0; j < _ngated; j++) {
            if (_gated_devs[j].mmio_base == vm->dev[i].mmio_base) {
                _gated_devs[j].gated = false;
            }
        }
    }

    _state[vm->id - 1] = POWER_STATE_ON;
    LOG_INFO("Power: VM%u ungated", vm->id);
    return E_OK;
}

power_state_t power_get_state(u32 vm_id)
{
    if (vm_id == 0 || vm_id > MAX_VMS) return POWER_STATE_ON;
    return _state[vm_id - 1];
}

bool power_is_device_gated(u64 mmio_base)
{
    for (u32 i = 0; i < _ngated; i++) {
        if (_gated_devs[i].mmio_base == mmio_base && _gated_devs[i].gated)
            return true;
    }
    return false;
}
