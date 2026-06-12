/*
 * platform_init.c — Create Linux + RTOS + Android VMs
 *
 * Memory map matches the QEMU -device loader addresses:
 *   Linux:   -device loader,...,addr=0x41200000  -> entry_pa=0x41200000
 *   RTOS:    -device loader,...,addr=0x46008000  -> entry_pa=0x46008000
 *   Android: -device loader,...,addr=0x48000000  -> entry_pa=0x48000000
 *
 * The hypervisor identity-maps each guest's IPA=0 to its PA base,
 * so the guest linker script VMA matches the IPA.
 */
#include "../include/hypervisor.h"
#include "../include/config.h"
#include "../include/error.h"
#include "../core/vm/vm.h"
#include "../vre/irq/irq_router.h"
#include "../vre/device/mmio_trap.h"
#include "../guest/shmem/shmem.h"
#include "../guest/ipc/ipc.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"
#include "pal/pal.h"
#include "dtb/dtb.h"

err_t platform_init_vms(void);

err_t vm_subsys_init(void)
{
    err_t e;
    e = irq_router_init(); if (FAIL(e)) return e;
    e = shmem_init();      if (FAIL(e)) return e;
    e = ipc_init();        if (FAIL(e)) return e;
    return platform_init_vms();
}

/* Emulated UART write handler — forward guest UART output to
   the hypervisor's own UART so it appears on the console */
static err_t uart_emu(u64 addr, bool wr, u64 *val, void *priv)
{
    UNUSED(priv); UNUSED(addr);
    if (wr) {
        extern void uart_putc(char);
        uart_putc((char)(*val & 0xFF));
    } else {
        *val = 0x90;   /* TX FIFO not full, RX FIFO empty */
    }
    return E_OK;
}

err_t platform_init_vms(void)
{
    err_t e;
    vm_t *vm;

    /*
     * ============================================================
     * Linux VM
     * ============================================================
     *
     * Guest Image:
     *   PA = 0x41200000
     *
     * Guest RAM:
     *   IPA 0x00000000 -> PA 0x41000000
     *   Size = 496 MB
     *
     * Guest sees:
     *   kernel entry = IPA 0x00200000
     *
     * DTB:
     *   IPA 0x02000000
     *   PA  0x43000000
     */

    e = vm_create("linux", VM_LINUX, &vm);
    if (FAIL(e)) return e;

    vm->num_vcpus = 1;

    /* Guest sees kernel at IPA 0x00200000 */
    vm->entry_ipa = 0x00000000ULL;   // standard ARM64 2MB-aligned entry

    /* Guest sees DTB at IPA 0x02000000 */
    vm->dtb_ipa = 0x04000000ULL;

/*
 * Map guest IPA 0x0
 * to host PA 0x41000000
 */
    /* RAM region 1: IPA 0x00000000 – 0x07FFFFFF (128 MB, below GIC) */
    e = vm_add_mem(vm,
               0x41000000ULL,
               0x00000000ULL,
               0x08000000ULL,
               MEM_R|MEM_W|MEM_X);
    if (FAIL(e)) return e;

    /* RAM region 2: IPA 0x10000000 – 0x4FFFFFFF (1 GB, above all devices) */
    e = vm_add_mem(vm,
               0x51000000ULL,
               0x10000000ULL,
               0x40000000ULL,
               MEM_R|MEM_W|MEM_X);
    if (FAIL(e)) return e;

    /*
     * UART passthrough
     */
    e = vm_add_mem(
            vm,
            UART_BASE_QEMU,
            UART_BASE_QEMU,
            0x1000ULL,
            MEM_R | MEM_W | MEM_IO);

    if (FAIL(e)) return e;

    /*
     * GIC interface
     */
    e = vm_add_mem(
            vm,
            0x08000000ULL,   /* GICv3 distributor base */
            0x08000000ULL,
            0x10000ULL,
            MEM_R | MEM_W | MEM_IO);
           
    if (FAIL(e)) return e;
    /* GICv3 redistributor — Linux needs this, not the v2 CPU interface */
    e = vm_add_mem(vm, 0x080A0000ULL, 0x080A0000ULL,
               0x00F60000ULL, MEM_R | MEM_W | MEM_IO);
    if (FAIL(e)) return e;
        /*
         * UART device assignment
        */
    /*e = vm_add_dev(vm,
                   UART_BASE_QEMU,
                   0x1000ULL,
                   33,
                   true);

    if (FAIL(e)) return e;*/

    /*
     * UART MMIO emulation
     */
   /* mmio_register(UART_BASE_QEMU,
                  0x1000ULL,
                  uart_emu,
                  NULL);*/
/* UART is passthrough via vm_add_mem — no emulation needed */
    /*
     * Build Stage-2 tables
     */
    LOG_INFO("Linux kernel spans IPA 0x%lx to 0x%lx",
         0x00200000ULL,
         0x00200000ULL + 0x24f0000ULL);
    LOG_INFO("Maps to PA 0x%lx to 0x%lx", 
         0x41200000ULL,
         0x41200000ULL + 0x24f0000ULL);
    LOG_INFO("End of mapping PA 0x%lx, end of region PA 0x%lx",
         0x41200000ULL + 0x24f0000ULL,
         0x41000000ULL + 0x1F000000ULL);
    e = vm_finalize(vm);

    if (FAIL(e)) return e;

    /*
     * Build Linux DTB
     */
    {
        static u8 dtb_buf[8192];

        u64 dtb_sz = 0;

        err_t de = dtb_build_linux(
                        dtb_buf,
                        sizeof(dtb_buf),
                        0x00000000ULL,
                        0x08000000ULL,   /* first region: 128MB below GIC */
                        UART_BASE_QEMU,
                        33,
                        &dtb_sz);
        if (OK(de)) {
            u8 *dtb_dst = (u8*)(uintptr_t)(0x45000000ULL);
            memcpy(dtb_dst, dtb_buf, dtb_sz);
            LOG_INFO("DTB built: %ld bytes at PA 0x45000000", dtb_sz);
        }
    }

    /*
     * Start VM
     */
    e = vm_start(vm);

    if (FAIL(e)) return e;

    LOG_INFO(
        "Platform init: %d VM configured",
        g_hyp.num_vms);

    return E_OK;
}
