/*
 * platform_init.c — QEMU virt platform initialisation (Phase 2)
 *
 * Phase 2 additions vs Phase 1:
 *
 *   1. platform_phase2_init() called at top of platform_init() to
 *      initialise power_manager and smmu (graceful no-op on QEMU).
 *
 *   2. Memory and device regions are now applied via device_profile_init()
 *      from boot/device_profile.c instead of hardcoded vm_add_mem calls.
 *      This mirrors what the YAML configs describe.
 *
 *   3. mmio_register_vm_device() is called inside device_profile_init()
 *      for every MEM_IO region, so MMIO ownership is enforced.
 *
 * External API: same as Phase 1.
 *   platform_init() is called from vm_subsys_init() in core/vm/vm.c.
 */

#include "../include/hypervisor.h"
#include "../include/config.h"
#include "../include/error.h"
#include "../include/types.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"
#include "../core/vm/vm.h"
#include "../drivers/gic/gicv3.h"
#include "../vre/irq/irq_router.h"
#include "../vre/dma/smmu.h"
#include "../vre/bus/pcie.h"   /* Phase 4D */
#include "../vre/power/power.h"
#include "../boot/device_profile.h"
#include "../boot/pal/pal.h"   /* Phase 4C: g_plat.smmu_base */

/* Guest load / entry addresses */
#define LINUX_ENTRY_IPA       0x00200000ULL
#define LINUX_DTB_IPA         0x04000000ULL
#define RTOS_ENTRY_IPA        0x00008000ULL
#define ANDROID_ENTRY_IPA     0x00200000ULL
#define ANDROID_DTB_IPA       0x02000000ULL

/* Linux physical memory layout (must match QEMU loader addresses) */
#define LINUX_RAM_PA_BASE     0x41000000ULL   /* -device loader addr for Image */
#define LINUX_RAM_SIZE        0x08000000ULL   /* 128 MB first region */
#define LINUX_INITRD_PA       0x47000000ULL   /* -device loader addr for initrd */
#define LINUX_DTB_PA          0x45000000ULL   /* physical address to write DTB */
#define LINUX_DTB_BUF_SZ      0x00010000ULL   /* 64 KB DTB buffer */
#define LINUX_INITRD_START_IPA (LINUX_INITRD_PA - LINUX_RAM_PA_BASE)
#ifndef INITRD_SIZE
#define INITRD_SIZE           0x00200000ULL   /* fallback if not set by Makefile */
#endif
#define LINUX_INITRD_END_IPA   (LINUX_INITRD_START_IPA + INITRD_SIZE)

/* GIC distributor local emulation stub (unused on QEMU passthrough) */
static err_t gic_dist_emulate_local(u64 addr, bool wr, u64 *val, void *priv)
{
    UNUSED(addr); UNUSED(wr); UNUSED(val); UNUSED(priv);
    return E_OK;
}

/* ── VM creation ── */
static err_t create_linux_vm(void)
{
    vm_t *vm;
    err_t e = vm_create("linux", VM_LINUX, &vm);
    if (FAIL(e)) return e;

    vm->entry_ipa = LINUX_ENTRY_IPA;
    vm->dtb_ipa   = LINUX_DTB_IPA;


    /* Build DTB for Linux and write to PA 0x45000000 */
    extern err_t dtb_build_linux(void *buf, u64 buf_sz,
                                  u64 ram_base, u64 ram_size,
                                  u64 uart_base, u32 uart_irq,
                                  u64 initrd_start_ipa, u64 initrd_end_ipa,
                                  u64 *out_sz);
    void *dtb_buf = (void *)(uintptr_t)LINUX_DTB_PA;
    u64   dtb_sz  = 0;
    e = dtb_build_linux(dtb_buf, LINUX_DTB_BUF_SZ,
                        LINUX_RAM_PA_BASE, LINUX_RAM_SIZE,
                        UART_BASE_QEMU, 33u,
                        LINUX_INITRD_START_IPA, LINUX_INITRD_END_IPA,
                        &dtb_sz);
    if (FAIL(e)) LOG_WARN("Linux DTB build failed (err=%d)", (int)e);
    else         LOG_INFO("Linux DTB built: %lu bytes at PA 0x%lx",
                          dtb_sz, LINUX_DTB_PA);

    LOG_INFO("Linux VM ready");
    return E_OK;
}

static err_t create_rtos_vm(void)
{
    vm_t *vm;
    err_t e = vm_create("rtos", VM_RTOS, &vm);
    if (FAIL(e)) return e;

    vm->entry_ipa = RTOS_ENTRY_IPA;
    vm->dtb_ipa   = 0;

    LOG_INFO("RTOS VM ready (entry IPA=0x%lx)", vm->entry_ipa);
    return E_OK;
}

static err_t create_android_vm(void)
{
    vm_t *vm;
    err_t e = vm_create("android", VM_ANDROID, &vm);
    if (FAIL(e)) return e;

    vm->entry_ipa = ANDROID_ENTRY_IPA;
    vm->dtb_ipa   = ANDROID_DTB_IPA;

    LOG_INFO("Android VM ready");
    return E_OK;
}

/* ── Main platform init (called from vm_subsys_init) ── */
err_t platform_init(void)
{
    err_t e;

    /* Phase 2: power manager */
    e = power_init();
    if (FAIL(e)) LOG_WARN("Power init failed (err=%d)", (int)e);
    else         LOG_INFO("Power manager ready");

    /* Phase 2: SMMU — graceful no-op on QEMU (no hardware) */
    smmu_init(g_plat.smmu_base);

    /* Phase 4D: PCIe bus manager — trap ECAM, assign VM1 all bus 0 devices */
    pcie_init();
    pcie_assign_vm(1, 0xFF, 0, 0);   /* VM1 (Linux) gets all PCIe devices */
    if (smmu_present())
        LOG_INFO("SMMU: hardware present and enabled");
    else
        LOG_INFO("SMMU: not present — DMA isolation via software guard only");

    /* Create the three VMs */
    e = create_linux_vm();   if (FAIL(e)) return e;
    e = create_rtos_vm();    if (FAIL(e)) return e;
    e = create_android_vm(); if (FAIL(e)) return e;

    /* Phase 2: apply device profile (vm_add_mem + mmio_register_vm_device) */
    e = device_profile_init();
    if (FAIL(e)) {
        LOG_ERROR("Device profile init failed (err=%d)", (int)e);
        return e;
    }

    /* Finalize and start all VMs */
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = g_hyp.vms[i];
        if (!vm) continue;

        e = vm_finalize(vm);
        if (FAIL(e)) {
            LOG_ERROR("vm_finalize VM%u failed (err=%d)", vm->id, (int)e);
            return e;
        }
        e = vm_start(vm);
        if (FAIL(e)) {
            LOG_ERROR("vm_start VM%u failed (err=%d)", vm->id, (int)e);
            return e;
        }
        /* Phase 4C: bind VM to SMMU stream table */
        if (smmu_present())
            smmu_assign_stream(vm->id - 1u, vm->id, vm->s2_pgd, vm->vttbr);
    }

    /* IRQ routing */
    e = irq_router_init();
    if (FAIL(e)) return e;

    e = irq_route_add(33, 1, 33);  /* UART SPI → Linux */
    if (FAIL(e)) LOG_WARN("irq_route_add(33) err=%d", (int)e);

    e = irq_route_add(34, 1, 34);  /* virtio-net → Linux */
    if (FAIL(e)) LOG_WARN("irq_route_add(34) err=%d", (int)e);

    e = irq_route_add(35, 1, 35);  /* virtio-blk → Linux */
    if (FAIL(e)) LOG_WARN("irq_route_add(35) err=%d", (int)e);

    LOG_INFO("IRQ routes registered: 3 entries");
    return E_OK;
}
