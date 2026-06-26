/*
 * main.c — Primary hypervisor C entry point
 *
 * Phase 1 fix: EC_WFI now calls sched_on_wfi() instead of sched_on_timer().
 *
 * sched_on_timer() with RT-budget policy re-selects RTOS when called
 * from the WFI trap, causing an infinite loop (RTOS WFI → trap →
 * sched picks RTOS → ERET → WFI → trap ...).
 *
 * sched_on_wfi() always switches to the next BE slot, breaking the loop.
 */

#include "../include/hypervisor.h"
#include "../include/types.h"
#include "../include/error.h"
#include "../include/config.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"
#include "pal/pal.h"
#include "../arch/arm64/include/arm_regs.h"
#include "../core/vcpu/vcpu.h"
#include "../core/vm/vm.h"

#define EC_WFI 0x01

hypervisor_t g_hyp;

extern void sched_on_timer(void);
extern void sched_on_wfi(void);       /* Phase 1: dedicated WFI handler */
err_t gic_init(u64 gicd, u64 gicr);
err_t vm_subsys_init(void);
err_t sched_init(void);
void  sched_run(void) __noreturn;
void  sched_secondary_run(u32 cpu_id) __noreturn;
void  psci_handler(u64 *regs);
void  hvc_dispatch(void *regs);
void  fault_dabt(void *regs, u64 esr);
void  fault_iabt(void *regs, u64 esr);

void hyp_main(u32 cpu_id, paddr_t dtb_pa)
{
    err_t e;
    memset(&g_hyp, 0, sizeof(g_hyp));

    e = pal_init(dtb_pa);
    if (FAIL(e)) { while(1) WFI(); }

    LOG_INFO("====================================");
    LOG_INFO("  Tessolve Hypervisor v1.0  booting");
    LOG_INFO("  Platform : %s", g_plat.name);
    LOG_INFO("  CPU ID   : %d", (int)cpu_id);
    LOG_INFO("  DTB PA   : 0x%lx", dtb_pa);
    LOG_INFO("====================================");

    g_hyp.timer_freq_hz = READ_SYSREG(cntfrq_el0);
    g_hyp.num_cpus = 1;
    LOG_INFO("Timer freq: %ld Hz", g_hyp.timer_freq_hz);

    LOG_INFO("Init GICv3...");
    e = gic_init(g_plat.gicd_base, g_plat.gicr_base);
    LOG_INFO("Init PMU...");
    extern void pmu_init(void);
    pmu_init();
    if (FAIL(e)) hyp_panic("GIC init failed");

    LOG_INFO("Init VM subsystem...");
    e = vm_subsys_init();
    if (FAIL(e)) hyp_panic("VM subsys init failed");

    LOG_INFO("Init scheduler...");
    e = sched_init();
    if (FAIL(e)) hyp_panic("Scheduler init failed");

    g_hyp.initialized = true;
    LOG_INFO("Hypervisor ready — starting scheduler");

    sched_run();
}

void hyp_secondary_main(u32 cpu_id)
{
    LOG_INFO("Secondary CPU %d online", (int)cpu_id);
    g_hyp.num_cpus++;
    sched_secondary_run(cpu_id);
}

void hyp_sync_handler(void *regs, u64 esr)
{
    switch (ESR_EC(esr)) {
    case EC_HVC64:  hvc_dispatch(regs);       break;
    case EC_SMC64:  psci_handler((u64*)regs);  break;
    case EC_DABT_L: fault_dabt(regs, esr);    break;
    case EC_IABT_L: fault_iabt(regs, esr);    break;

    case EC_WFI:
        /*
         * Guest executed WFI (idle).
         *
         * MUST call sched_on_wfi(), NOT sched_on_timer().
         *
         * sched_on_timer() with RT-budget would re-select RTOS,
         * causing ERET → WFI → trap → RTOS again — an infinite loop
         * that starves Linux of CPU permanently.
         *
         * sched_on_wfi() advances ELR, resets the RT budget, and
         * forces a switch to the next BE (Linux/Android) slot.
         */
        sched_on_wfi();
        break;

    case 0x18: {    /* MSR/MRS system register trap — ICC (4B) + PMU (4A) */
        extern err_t pmu_handle_trap(void *regs, u64 esr);
        /* Try ICC registers first (icc_pmr_el1, icc_grpen1_el1) */
        err_t _handled = E_UNSUPPORTED;
        if (g_current_vcpu[0])
            _handled = vgic_icc_trap(&g_current_vcpu[0]->vgic_cpu, regs, esr);
        /* Fall through to PMU handler for PMU registers */
        if (FAIL(_handled))
            pmu_handle_trap(regs, esr);
        /* MUST advance ELR past the trapped MRS/MSR instruction */
        if (g_current_vcpu[0])
            g_current_vcpu[0]->regs.elr_el2 += 4;
        break;
    }
    default:
        LOG_INFO("SYNC EC=0x%x ESR=0x%lx ELR=0x%lx",
                 (int)ESR_EC(esr), esr,
                 g_current_vcpu[0] ? g_current_vcpu[0]->regs.elr_el2 : 0ULL);
    }
}

void hyp_irq_handler(void *regs)
{
    UNUSED(regs);
    extern void gic_handle_irq(void *regs);
    gic_handle_irq(NULL);
}

void hyp_panic(const char *msg)
{
    LOG_ERROR("** PANIC: %s", msg);
    asm volatile("msr daifset, #0xF");
    while (1) WFI();
}
