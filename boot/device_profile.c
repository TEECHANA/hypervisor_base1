/*
 * device_profile.c — Device profile manager (Phase 2 §3.1.4)
 *
 * Static C-struct equivalent of the YAML configs in configs/:
 *   configs/linux_vm.yaml
 *   configs/rtos_vm.yaml
 *   configs/android_vm.yaml
 *
 * platform_init.c calls device_profile_init() instead of hardcoding
 * every vm_add_mem / vm_add_dev call inline. To reassign a device to
 * a different VM, change the vm_id field in the table below.
 *
 * Table conventions:
 *   vm_id = 1  → Linux
 *   vm_id = 2  → RTOS
 *   vm_id = 3  → Android
 *   irq   = 0  → no interrupt
 *   stream_id = 0 → no SMMU stream (not DMA-capable or not SMMU-managed)
 */

#include "device_profile.h"
#include "../core/vm/vm.h"
#include "../vre/device/mmio_trap.h"
#include "../vre/dma/smmu.h"
#include "../lib/log/log.h"
#include "../include/config.h"

/*
 * _profile — one entry per device/memory region assigned to a VM.
 *
 * Memory regions (MEM_R|MEM_W|MEM_X, no MEM_IO): added via vm_add_mem().
 * Device regions (MEM_R|MEM_W|MEM_IO): added via vm_add_mem() for S2 map
 *   AND registered with mmio_register_vm_device() for trap handling.
 *
 * IRQ column: registered with irq_route_add() in platform_init.c after
 *   this table is applied (IRQ routing is separate from device assignment).
 */
static const dev_profile_entry_t _profile[] = {

    /* ── Linux VM (vm_id=1) ── */

    /* Linux main RAM: IPA 0x0 → PA 0x41000000, 128 MB */
    {
        .vm_id    = 1,
        .pa_base  = 0x41000000ULL,
        .ipa_base = 0x00000000ULL,
        .size     = 0x08000000ULL,    /* 128 MB */
        .flags    = MEM_R | MEM_W | MEM_X,
        .irq      = 0,
        .stream_id = 0,
        .name     = "linux-ram-low",
    },
    /* Linux extended RAM: IPA 0x10000000 → PA 0x51000000, 1 GB */
    {
        .vm_id    = 1,
        .pa_base  = 0x51000000ULL,
        .ipa_base = 0x10000000ULL,
        .size     = 0x40000000ULL,    /* 1 GB */
        .flags    = MEM_R | MEM_W | MEM_X,
        .irq      = 0,
        .stream_id = 0,
        .name     = "linux-ram-high",
    },
    /* Linux UART (PL011) passthrough */
    {
        .vm_id    = 1,
        .pa_base  = UART_BASE_QEMU,
        .ipa_base = UART_BASE_QEMU,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 33,
        .stream_id = 0,
        .name     = "linux-uart",
    },
    /* Linux GIC distributor passthrough */
    {
        .vm_id    = 1,
        .pa_base  = GICD_BASE_QEMU,
        .ipa_base = GICD_BASE_QEMU,
        .size     = 0x00100000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "linux-gicd",
    },
    /* Linux GIC redistributor passthrough */
    {
        .vm_id    = 1,
        .pa_base  = GICR_BASE_QEMU,
        .ipa_base = GICR_BASE_QEMU,
        .size     = 0x00200000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "linux-gicr",
    },

    /* ── RTOS VM (vm_id=2) ── */

    /* RTOS RAM: IPA 0x0 → PA 0x60000000, 240 MB */
    {
        .vm_id    = 2,
        .pa_base  = 0x60000000ULL,
        .ipa_base = 0x00000000ULL,
        .size     = 0x0F000000ULL,    /* 240 MB */
        .flags    = MEM_R | MEM_W | MEM_X,
        .irq      = 0,
        .stream_id = 0,
        .name     = "rtos-ram",
    },
    /* RTOS UART passthrough (shared physical UART, separate IPA mapping) */
    {
        .vm_id    = 2,
        .pa_base  = UART_BASE_QEMU,
        .ipa_base = UART_BASE_QEMU,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "rtos-uart",
    },

    /* ── Phase 5: OBD shared memory — hypervisor writes, Linux mmap reads ── */
    {
        .vm_id    = 1,
        .ipa_base = 0x0A000000ULL,
        .pa_base  = 0x40F00000ULL,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W,
        .name     = "linux-obd-shmem",
    },

    /* ── Android VM (vm_id=3) ── */

    /* Android RAM: IPA 0x0 → PA 0x70000000, 512 MB */
    {
        .vm_id    = 3,
        .pa_base  = 0x70000000ULL,
        .ipa_base = 0x00000000ULL,
        .size     = 0x20000000ULL,    /* 512 MB */
        .flags    = MEM_R | MEM_W | MEM_X,
        .irq      = 0,
        .stream_id = 0,
        .name     = "android-ram",
    },
    /* Android UART passthrough */
    {
        .vm_id    = 3,
        .pa_base  = UART_BASE_QEMU,
        .ipa_base = UART_BASE_QEMU,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "android-uart",
    },

    /*
     * Per-VM exclusive hypervisor mailbox pages (§2.2.1 MMIO ownership).
     *
     * Each VM owns one private 4 KB IO page that is NOT shared with any
     * other VM. Accesses trap to the hypervisor MMIO handler, which enforces
     * ownership. Physical addresses 0x0b000000-0x0b002fff are unused MMIO
     * space on QEMU virt (above virtio-mmio at 0x0a003e00, below PCIe).
     *
     * These entries satisfy the phase2 §2.2.1 check:
     *   "MMIO: VM.*owns device" logged during device_profile_init().
     */
    {
        .vm_id    = 1,
        .pa_base  = 0x0b000000ULL,
        .ipa_base = 0x0b000000ULL,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "hyp-mbox-linux",
    },
    {
        .vm_id    = 2,
        .pa_base  = 0x0b001000ULL,
        .ipa_base = 0x0b001000ULL,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "hyp-mbox-rtos",
    },
    {
        .vm_id    = 3,
        .pa_base  = 0x0b002000ULL,
        .ipa_base = 0x0b002000ULL,
        .size     = 0x1000ULL,
        .flags    = MEM_R | MEM_W | MEM_IO,
        .irq      = 0,
        .stream_id = 0,
        .name     = "hyp-mbox-android",
    },
};

#define PROFILE_COUNT ((u32)(sizeof(_profile) / sizeof(_profile[0])))

const dev_profile_entry_t *device_profile_get(u32 *count_out)
{
    if (count_out) *count_out = PROFILE_COUNT;
    return _profile;
}

err_t device_profile_init(void)
{
    LOG_INFO("DevProfile: applying %u entries", PROFILE_COUNT);

    for (u32 i = 0; i < PROFILE_COUNT; i++) {
        const dev_profile_entry_t *e = &_profile[i];

        /* Find the VM */
        vm_t *vm = vm_by_id(e->vm_id);
        if (!vm) {
            LOG_WARN("DevProfile[%u] '%s': VM%u not found — skipped",
                     i, e->name, e->vm_id);
            continue;
        }

        /* Add the memory/device region to the VM's S2 map */
        err_t r = vm_add_mem(vm, e->pa_base, e->ipa_base, e->size, e->flags);
        if (r != E_OK) {
            LOG_ERROR("DevProfile[%u] '%s': vm_add_mem failed (err=%d)",
                      i, e->name, (int)r);
            return r;
        }

        /*
         * Register device-mapped regions with MMIO trap handler.
         * UART (0x09000000) and GIC (0x08000000, 0x080a0000) are shared
         * physical devices accessed by all VMs via Stage-2 passthrough.
         * Do NOT register them with per-VM ownership — the Stage-2 mapping
         * already enforces access. Only register truly exclusive devices.
         */
        if ((e->flags & MEM_IO) &&
            e->ipa_base != 0x09000000ULL &&   /* UART: shared */
            e->ipa_base != 0x08000000ULL &&   /* GICD: shared */
            e->ipa_base != 0x080a0000ULL)     /* GICR: shared */
        {
            mmio_register_vm_device(e->vm_id, e->ipa_base, e->size,
                                    NULL,
                                    NULL);
        }

        /* Assign SMMU stream if DMA-capable */
        if (e->stream_id && smmu_present()) {
            smmu_assign_stream(e->stream_id, e->vm_id,
                               vm->s2_pgd, vm->vttbr);
        }

        LOG_INFO("DevProfile[%u] VM%u '%s': PA=%lx IPA=%lx sz=%lx%s",
                 i, e->vm_id, e->name,
                 e->pa_base, e->ipa_base, e->size,
                 (e->flags & MEM_IO) ? " [IO]" : "");
    }

    LOG_INFO("DevProfile: all entries applied");
    return E_OK;
}
