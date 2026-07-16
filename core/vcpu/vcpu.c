/*
 * vcpu.c — Virtual CPU management (Phase 4A: adds PMU save/restore)
 *
 * Phase 4A change: vcpu_do_switch() now calls pmu_save(prev) and
 * pmu_restore(next) around the sysreg switch. This gives each VM
 * its own private PMU view — Linux can count its own cycles without
 * interference from RTOS or Android.
 *
 * vcpu_init() calls pmu_vcpu_init() to zero the PMU context.
 */

#include "vcpu.h"
#include "pmu.h"
#include "../vm/vm.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../arch/arm64/include/arm_regs.h"

vcpu_t *g_current_vcpu[MAX_PHYS_CPUS];

err_t vcpu_init(vcpu_t *vc, struct vm *vm, u32 id)
{
    if (!vc || !vm) return E_INVAL;
    memset(vc, 0, sizeof(*vc));

    vc->vcpu_id   = id;
    vc->vm        = vm;
    vc->state     = VCPU_READY;
    vc->vttbr_el2 = vm->vttbr;

    /* EL1 reset state */
    vc->sysregs.sctlr_el1     = 0x00000000ULL;
    vc->sysregs.cpacr_el1     = (3ULL << 20);
    vc->sysregs.cntkctl_el1   = 0ULL;
    vc->sysregs.mair_el1      = 0xFFULL;
    vc->sysregs.tcr_el1       = 0ULL;
    vc->sysregs.ttbr0_el1     = 0ULL;
    vc->sysregs.ttbr1_el1     = 0ULL;

    vc->sysregs.cntv_ctl_el0  = 0ULL;
    vc->sysregs.cntv_cval_el0 = 0ULL;
    vc->sysregs.ich_lr0       = 0ULL;

    __asm__ volatile("msr cntvoff_el2, xzr");

    vc->regs.elr_el2  = vm->entry_ipa;
    vc->regs.spsr_el2 = SPSR_EL1H;
    vc->regs.x[0]     = vm->dtb_ipa;
    vc->regs.x[1]     = 0ULL;
    vc->regs.x[2]     = 0ULL;
    vc->regs.x[3]     = 0ULL;

    /* Phase 4A: initialise PMU context */
    pmu_vcpu_init(&vc->pmu);

    LOG_INFO("vCPU %d init VM %d", id, vm->id);
    LOG_INFO("  entry IPA : %lx", vm->entry_ipa);
    LOG_INFO("  DTB IPA   : %lx", vm->dtb_ipa);
    LOG_INFO("  VTTBR     : %lx", vm->vttbr);
    return E_OK;
}

void vcpu_set_entry(vcpu_t *vc, paddr_t entry, u64 arg0)
{
    vc->reset_pc      = entry;
    vc->reset_x0      = arg0;
    vc->regs.elr_el2  = entry;
    vc->regs.spsr_el2 = SPSR_EL1H;
    vc->regs.x[0]     = arg0;
}

void vcpu_stop(vcpu_t *vc)
{
    if (vc) vc->state = VCPU_STOPPED;
}

/*
 * vcpu_do_switch — perform a full vCPU context switch.
 *
 * Called from sched.c do_switch() when prev != next.
 * GPRs are handled by entry.S macros (SAVE_GUEST_REGS / RESTORE_GUEST_REGS).
 * This function handles:
 *   1. EL1 system registers (sysregs)
 *   2. PMU registers (Phase 4A)
 *   3. VTTBR_EL2 (Stage-2 page table base for the next VM)
 */
void vcpu_do_switch(vcpu_t *prev, vcpu_t *next)
{
    if (!next) return;

    if (prev) {
        /* Save prev EL1 sysregs */
        vcpu_save_sysregs(&prev->sysregs);

        /* Phase 4A: save prev PMU state */
        pmu_save(&prev->pmu);

        prev->state = VCPU_READY;
    }

    /* Restore next EL1 sysregs */
    vcpu_restore_sysregs(&next->sysregs);

    /* Phase 4A: restore next PMU state */
    pmu_restore(&next->pmu);

    /* Switch Stage-2 page tables */
    __asm__ volatile(
        "msr vttbr_el2, %0\n"
        "isb\n"
        :: "r"(next->vttbr_el2) : "memory"
    );

    next->state        = VCPU_RUNNING;
    g_current_vcpu[0]  = next;
}
