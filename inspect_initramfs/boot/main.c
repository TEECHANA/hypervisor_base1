/*
 * main.c — Primary hypervisor C entry point
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

hypervisor_t g_hyp;

/* Forward declarations */
err_t gic_init(u64 gicd, u64 gicr);
err_t vm_subsys_init(void);
err_t sched_init(void);
void  sched_run(void)  __noreturn;
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

/*
 * hyp_sync_handler — called from _guest64_sync in entry.S
 * x0 = pointer to current vcpu->regs (= g_current_vcpu[0])
 * x1 = ESR_EL2
 */
void hyp_sync_handler(void *regs, u64 esr)
{
    switch (ESR_EC(esr)) {
    case EC_HVC64:  hvc_dispatch(regs);       break;
    case EC_SMC64:  psci_handler((u64*)regs); break;
    case EC_DABT_L: fault_dabt(regs, esr);    break;
    case EC_IABT_L: fault_iabt(regs, esr);    break;
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
    LOG_ERROR("*** PANIC: %s", msg);
    __asm__ volatile("msr daifset, #0xF");
    while (1) WFI();
}
