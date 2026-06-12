/*
 * sched.c — Static cyclic-executive time-partition scheduler
 *
 * Design:
 *   A table of (vm_id, vcpu_id, duration_us) slots is round-robined.
 *   On each CNTHP timer IRQ (routed to EL2 via HCR_EL2.IMO):
 *     1. entry.S SAVE_GUEST_REGS saves current vCPU's GPRs
 *     2. hyp_irq_handler → gic_handle_irq → sched_on_timer
 *     3. sched_on_timer calls vcpu_do_switch(prev, next) which:
 *           - saves prev EL1 sysregs
 *           - updates g_current_vcpu[0] = next
 *           - restores next EL1 sysregs + VTTBR_EL2
 *     4. entry.S RESTORE_GUEST_REGS loads next vCPU's GPRs
 *     5. ERET enters next vCPU at the PC it was interrupted at
 *
 * Result: true hardware-level context switch on every timer tick.
 */
#include "sched.h"
#include "../vm/vm.h"
#include "../vcpu/vcpu.h"
#include "../../drivers/timer/timer.h"
#include "../../include/hypervisor.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../arch/arm64/include/arm_regs.h"

#define MAX_SLOTS  (MAX_VMS * MAX_VCPU_PER_VM)
#define HCR_COMMON \
    (1UL << 31) |   /* RW  = EL1 is AArch64 */ \
    (1UL << 4)  |   /* IMO = route IRQ to EL2 */ \
    (1UL << 3)  |   /* FMO = route FIQ to EL2 */ \
    (1UL << 2)  |   /* AMO = route SError to EL2 */ \
    (1UL << 0)      /* VM  = enable stage-2 translation */
static sched_slot_t _slots[MAX_SLOTS];
static u32          _nslots = 0;
static u32          _cur    = 0;

err_t sched_init(void)
{
    memset(_slots, 0, sizeof(_slots));
    _nslots = 0;
    _cur    = 0;

    LOG_INFO("Scheduler init start");
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = g_hyp.vms[i];
        if (!vm) continue;
        LOG_INFO("Adding VM %d '%s' with %d vCPUs", vm->id, vm->name, vm->num_vcpus);
        for (u32 v = 0; v < vm->num_vcpus; v++)
            sched_add_slot(vm->id, v, vm->sched.time_slice_us);
    }

    LOG_INFO("Scheduler ready: %d slots, major-frame=%d us", _nslots, SCHED_MAJOR_FRAME_US);
    return E_OK;
}

void sched_add_slot(u32 vm_id, u32 vcpu_id, u32 dur_us)
{
    if (_nslots >= MAX_SLOTS) return;
    _slots[_nslots].vm_id       = vm_id;
    _slots[_nslots].vcpu_id     = vcpu_id;
    _slots[_nslots].duration_us = dur_us ? dur_us : 1000;
    _nslots++;
}

/*
 * sched_on_timer — called from IRQ handler every timer tick.
 *
 * At entry: entry.S has already saved the interrupted vCPU's GPRs
 * into g_current_vcpu[0]->regs.
 *
 * We advance to the next slot and call vcpu_do_switch() which
 * updates g_current_vcpu[0] so that RESTORE_GUEST_REGS picks up
 * the new vCPU on the way out.
 */
void sched_on_timer(void)
{
    if (!_nslots) { timer_set_us(SCHED_MAJOR_FRAME_US); return; }

    /* Advance to next runnable slot */
    u32 start = _cur;
    do {
        _cur = (_cur + 1) % _nslots;
        vm_t  *vm = vm_by_id(_slots[_cur].vm_id);
        if (!vm || vm->state != VM_RUNNING) continue;
        u32 vid = _slots[_cur].vcpu_id;
        if (vid >= vm->num_vcpus) continue;
        vcpu_t *next = vm->vcpus[vid];
        if (!next || next->state == VCPU_STOPPED) continue;

        /* Found a runnable slot */
        vcpu_t *prev = g_current_vcpu[0];
        if (prev != next)
            vcpu_do_switch(prev, next);

        timer_set_us(_slots[_cur].duration_us);
        return;
    } while (_cur != start);

    /* No runnable slot found — keep current, reset timer */
    timer_set_us(SCHED_MAJOR_FRAME_US);
}

/*
 * sched_run — first entry into guest world; never returns.
 *
 * Called after all VMs are started. We select the first runnable
 * slot, set g_current_vcpu[0], program the timer, load all EL1
 * context, and ERET into the first guest vCPU.
 */
void sched_run(void)
{
    if (!_nslots || !g_hyp.num_vms) {
        LOG_WARN("No VMs — idle loop");
        while (1) WFI();
    }

    LOG_INFO("Starting VMs");
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = g_hyp.vms[i];
        if (vm && vm->state == VM_READY) {
            LOG_INFO("Starting VM %d '%s'", vm->id, vm->name);
            vm_start(vm);
        }
    }

    /* Find first runnable slot */
    vcpu_t *first = NULL;
    u32 first_dur = SCHED_MAJOR_FRAME_US;
    for (u32 i = 0; i < _nslots; i++) {
        vm_t *vm = vm_by_id(_slots[i].vm_id);
        if (!vm || vm->state != VM_RUNNING) continue;
        if (_slots[i].vcpu_id >= vm->num_vcpus) continue;
        first = vm->vcpus[_slots[i].vcpu_id];
        first_dur = _slots[i].duration_us;
        _cur = i;
        break;
    }
    if (!first) hyp_panic("No runnable vCPU found");

    /* Set global current vCPU pointer — entry.S RESTORE_GUEST_REGS reads this */
    g_current_vcpu[0] = first;
    first->state = VCPU_RUNNING;

    LOG_INFO("----------------------------------");
    LOG_INFO("Launching first guest");
    LOG_INFO("VM ID        : %d",   first->vm->id);
    LOG_INFO("VM Name      : %s",   first->vm->name);
    LOG_INFO("vCPU ID      : %d",   first->vcpu_id);
    LOG_INFO("Entry PC     : 0x%lx", first->regs.elr_el2);
    LOG_INFO("SPSR_EL2     : 0x%lx", first->regs.spsr_el2);
    LOG_INFO("DTB arg (x0) : 0x%lx", first->regs.x[0]);
    LOG_INFO("VTTBR_EL2    : 0x%lx", first->vttbr_el2);

    /* Verify first instruction is readable */
    /*
    u32 *pc = (u32*)(uintptr_t)first->regs.elr_el2;
    LOG_INFO("Guest first instruction PA=0x%lx = 0x%lx",
             first->regs.elr_el2, (u64)*pc); */
    LOG_INFO("Guest entry IPA = 0x%lx",
    	     first->regs.elr_el2);
    LOG_INFO("----------------------------------");

    /* Program timer for first slice */
    timer_set_us(first_dur);

    /* Load EL1 sysregs for first vCPU */
    vcpu_restore_sysregs(&first->sysregs);
    /* Configure EL2 memory attributes */
    u64 mair =
        (0x00ULL << 0) |
        (0xFFULL << 8);

    WRITE_SYSREG(mair_el2, mair);
    ISB();
    __asm__ volatile(
        "msr vtcr_el2, %0\n"
        "isb\n"
        ::
        "r"(VTCR_DEFAULT)
        : "memory");
        
    /* Switch VTTBR_EL2 */
    __asm__ volatile(
        "msr vttbr_el2, %0\n"
        "isb\n"
        :: "r"(first->vttbr_el2) : "memory");

    /* Set HCR_EL2 for guest (VM=1, RW=1, IMO/FMO/AMO=1, TSC=1) */
    __asm__ volatile(
        "msr hcr_el2, %0\n"
        "isb\n"
        :: "r"((u64)HCR_GUEST) : "memory");

    LOG_INFO("Executing ERET -> Guest");

    u64 *k = (u64*)(uintptr_t)0x41200000ULL;

    LOG_INFO("K0 = 0x%x", k[0]);
    LOG_INFO("K1 = 0x%x", k[1]);
    LOG_INFO("K2 = 0x%x", k[2]);
    LOG_INFO("K3 = 0x%x", k[3]);
    LOG_INFO("VTCR_EL2 = 0x%lx", READ_SYSREG(vtcr_el2));
    LOG_INFO("HCR_EL2  = 0x%lx", READ_SYSREG(hcr_el2));
    LOG_INFO("MAIR_EL2 = 0x%lx", READ_SYSREG(mair_el2));
    LOG_INFO("SCTLR_EL2= 0x%lx", READ_SYSREG(sctlr_el2));
    /* ERET: load ELR_EL2 + SPSR_EL2 from first vcpu and enter EL1 */
    __asm__ volatile(
        "msr elr_el2,  %0\n"
        "msr spsr_el2, %1\n"
        "ldp x0, x1,   [%2, #(0*8)]\n"   /* restore x0 (DTB) and x1 */
        
        "eret\n"
        :: "r"(first->regs.elr_el2),
           "r"(first->regs.spsr_el2),
           "r"(&first->regs.x[0])
        : "x0", "x1");

    __builtin_unreachable();
}

void sched_secondary_run(u32 cpu_id)
{
    UNUSED(cpu_id);
    while (1) WFI();
}
