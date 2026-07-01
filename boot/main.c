/*
 * main.c — Primary hypervisor C entry point
 *
 * Phase 1 fix: EC_WFI now calls sched_on_wfi() instead of sched_on_timer().
 *
 * VSE Phase 1: config_check_init() is called immediately after pal_init()
 * and before any VM is created. If the configuration HMAC does not match
 * the golden value, hyp_panic() fires here — no VM ever runs on a
 * tampered hypervisor.
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
#include "../vse/config_check.h"   /* VSE Phase 1 */
#include "../vse/component_check.h"   /* VSE Phase 2 */
#include "../vse/trust.h"            /* VSE Phase 3 */
#include "../vse/seal.h"             /* VSE Phase 4 */
#include "../vse/fault_detect.h"     /* VSE Phase 5 */
#include "../vse/failover.h"         /* VSE Phase 6 */
#include "../vse/totp.h"             /* Operator login: TOTP (RFC 6238) */
#include "../vse/login.h"            /* Operator login: 2FA prompt */
#include "../vse/ids.h"              /* IDS: runtime intrusion detection */
#include "../guest/ipc/ipc.h"        /* Inter-VM IPC subsystem */
#include "../core/sched/sched_stats.h" /* Scheduler accounting */
#include "../drivers/uart/uart.h"

#define EC_WFI 0x01

hypervisor_t g_hyp;

extern void sched_on_timer(void);
extern void sched_on_wfi(void);
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

    /*
     * VSE Phase 1 — Configuration integrity check.
     *
     * Must run before vm_subsys_init() so no VM is ever created
     * on a hypervisor whose configuration has been tampered with.
     *
     * In learn mode (CONFIG_CHECK_LEARN_MODE=1): logs the golden HMAC
     * and continues. In enforce mode: panics on mismatch.
     */
    LOG_INFO("VSE Phase 1: verifying hypervisor configuration...");
    e = config_check_init();
    if (FAIL(e)) {
        /* config_check_init() should have panicked — this is a safety net */
        hyp_panic("VSE: config_check_init returned failure without panic");
    }
    LOG_INFO("VSE Phase 1: configuration verified");

    /*
     * VSE Phase 2 — Component integrity check.
     *
     * Measures the live .text and .rodata sections and compares each
     * against a golden HMAC. Detects tampering of the hypervisor image
     * itself (patched code, corrupted constant tables).
     *
     * Runs after Phase 1 and before any VM is created. In learn mode it
     * logs the per-component golden HMACs; in enforce mode it panics on
     * mismatch.
     */
    LOG_INFO("VSE Phase 2: verifying hypervisor components...");
    e = component_check_init();
    if (FAIL(e)) {
        hyp_panic("VSE: component_check_init returned failure without panic");
    }
    LOG_INFO("VSE Phase 2: components verified");

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

    /* IPC subsystem — inter-VM messaging, must be after VMs are created */
    e = ipc_init();
    if (FAIL(e)) LOG_WARN("IPC: init failed (err=%d)", (int)e);

    /* Scheduler stats — init here so the log line appears before login */
    sched_stats_init();

    /*
     * VSE Phase 3 — Trust services.
     * Runs after VMs are created so it can mark each one TRUSTED.
     * Later phases (fault detection, IDS) feed trust_report_fault().
     */
    LOG_INFO("VSE Phase 3: initializing trust services...");
    e = trust_init();
    if (FAIL(e)) LOG_WARN("VSE Phase 3: trust_init failed (err=%d)", (int)e);
    else         LOG_INFO("VSE Phase 3: trust services initialized");

    /*
     * VSE Phase 4 — Sealing service.
     * Derives a platform-bound seal key from the verified config, then runs
     * a self-test (seal/unseal round-trip + tamper-detection) to prove the
     * subsystem works on this build.
     */
    LOG_INFO("VSE Phase 4: initializing sealing service...");
    e = seal_init();
    if (FAIL(e)) {
        LOG_WARN("VSE Phase 4: seal_init failed (err=%d)", (int)e);
    } else {
        seal_selftest();   /* logs PASS/FAIL; non-fatal */
        LOG_INFO("VSE Phase 4: sealing service initialized");
    }

    /*
     * VSE Phase 5 — Fault detection & recovery.
     * Activates the hooks that feed guest faults (memory, DMA, IRQ) into
     * the Phase 3 trust engine. Must run after trust_init() (Phase 3) so the
     * trust state machine exists when the first fault arrives.
     */
    LOG_INFO("VSE Phase 5: initializing fault detection...");
    e = fault_detect_init();
    if (FAIL(e)) LOG_WARN("VSE Phase 5: fault_detect_init failed (err=%d)", (int)e);
    else         LOG_INFO("VSE Phase 5: fault detection initialized");
    LOG_INFO("VSE IDS: initializing intrusion detection...");
    e = ids_init();
    if (FAIL(e)) LOG_WARN("VSE IDS: ids_init failed (err=%d)", (int)e);
    /* DEMO: exercise the IDS so it produces audit records (remove later) */

    /*
     * VSE Phase 6 — Backup OS failover.
     * Registers critical VMs with a backup image. When the trust engine
     * quarantines such a VM (after Phase 5 detects enough faults), the
     * hypervisor restarts it from the backup instead of leaving it dead.
     *
     * Here we protect VM1 (Linux). The backup image is assumed resident at
     * the Linux load PA; entry IPA mirrors the normal Linux entry (0x200000).
     * Adjust backup_pa / entry to point at a dedicated backup image if you
     * load one separately.
     */
    LOG_INFO("VSE Phase 6: initializing failover service...");
    e = failover_init();
    if (FAIL(e)) {
        LOG_WARN("VSE Phase 6: failover_init failed (err=%d)", (int)e);
    } else {
        /* Register VM1 (Linux) as critical with a backup at its load PA. */
        /* VM2 (rtos): backup @0x90000000, live @0x60008000, size, entry 0x8000 */
        failover_register(2u, 0x90000000ULL, 0x60008000ULL, 4904ULL, 0x8000ULL);
        LOG_INFO("VSE Phase 6: failover service initialized");

#ifdef VSE_IDS_DEMO
    LOG_INFO("VSE IDS: running detection demo...");
    for (int i = 0; i < 6; i++)
        fdetect_dma_violation(2, 0xDEAD0000 + i, 0x1000, 1);   /* storm on VM2 */
    fdetect_mem_fault(3, 0xBEEF000, 0x0C, true);                /* perm fault VM3 */
    ids_poll();                                                 /* heartbeat + downgrade scan */
    ids_print_summary();
    ids_print_log();
#endif /* VSE_IDS_DEMO */
    }

    /*
     * Operator login — two-factor (password + TOTP, RFC 6238).
     *
     * Runs AFTER all VSE integrity/trust/seal phases so the platform has
     * proven its own integrity before any human is let in. TOTP uses
     * lib/rtc (ARM generic timer + provisioned epoch) as its time source —
     * no hardware RTC required. T = unix_now / 30; window ±1 step.
     *
     * On lockout (too many failed attempts) we panic rather than boot guests.
     */
    LOG_INFO("Operator login: initializing TOTP...");
    e = totp_init();
    if (FAIL(e)) LOG_WARN("Operator login: totp_init failed (err=%d)", (int)e);
    totp_selftest();   /* logs PASS/FAIL; non-fatal */

    e = login_authenticate();
    if (FAIL(e))
        hyp_panic("Operator login failed — system locked");

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
        sched_on_wfi();
        break;

    case 0x18: {
        extern err_t pmu_handle_trap(void *regs, u64 esr);
        err_t _handled = E_UNSUPPORTED;
        if (g_current_vcpu[0])
            _handled = vgic_icc_trap(&g_current_vcpu[0]->vgic_cpu, regs, esr);
        if (FAIL(_handled))
            pmu_handle_trap(regs, esr);
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
