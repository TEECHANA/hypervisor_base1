/*
 * sched.c — Round-robin scheduler with Phase 3 visibility (Phase 3 §3.3.2/3.3.3)
 *
 * Phase 3 additions:
 *
 * 1. Context switch logging:
 *    Every switch prints: HYP [INF] CTX[N]: rtos→linux (1ms slice, timer)
 *    so the operator can see exactly which VM is running and why it switched.
 *
 * 2. Per-VM CPU time accounting:
 *    sched_stats_on_switch() is called on every switch to update
 *    total_us, slice_count, preempt_count per VM.
 *    Every 100 switches a utilisation report is printed automatically.
 *
 * 3. Configurable time slices:
 *    sched_set_slice(vm_id, us) updates the slot duration at runtime.
 *    The new duration takes effect on the NEXT timer tick for that VM.
 *    Accessible via HVC_SCHED_SET_SLICE hypercall from any guest.
 *
 * 4. WFI yield accounting:
 *    sched_on_wfi() now records the yield in sched_stats before switching.
 */

#include "sched.h"
#include "sched_stats.h"
#include "../vm/vm.h"
#include "../vcpu/vcpu.h"
#include "../../drivers/timer/timer.h"
#include "../../include/hypervisor.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../arch/arm64/include/arm_regs.h"
#include "../../vse/ids.h"             /* Audit fix #4: periodic ids_poll() */

#define MAX_SLOTS (MAX_VMS * MAX_VCPU_PER_VM)

/*
 * TWI (bit 13): guest WFI traps to EL2 via EC_WFI.
 * Required because QEMU's cortex-a57 does not reliably wake from WFI
 * via CNTHP when HCR_EL2.IMO=1 (confirmed in Phase 1 debug session).
 */
#define HCR_GUEST (HCR_VM | HCR_RW | HCR_IMO | HCR_FMO | HCR_AMO | HCR_TSC \
                   | (1UL << 13))  /* TWI: trap WFI to EL2 */

static sched_slot_t _slots[MAX_SLOTS];
static u32 _nslots = 0;
static u32 _cur    = 0;
bool g_sched_stopped = false;

/* ── Helpers ── */

static vcpu_t *slot_vcpu(u32 idx)
{
    vm_t *vm = vm_by_id(_slots[idx].vm_id);
    if (!vm || vm->state != VM_RUNNING) return NULL;
    u32 vid = _slots[idx].vcpu_id;
    if (vid >= vm->num_vcpus) return NULL;
    vcpu_t *vc = vm->vcpus[vid];
    if (!vc || vc->state == VCPU_STOPPED) return NULL;
    return vc;
}

static bool slot_is_rt(u32 idx)
{
    vm_t *vm = vm_by_id(_slots[idx].vm_id);
    return vm && vm->sched.realtime;
}

static void gate_uart(vcpu_t *next)
{
    extern void gic_enable_irq(u32);
    extern void gic_disable_irq(u32);
    if (next && next->vm && next->vm->id == 1) gic_enable_irq(33);
    else                                        gic_disable_irq(33);
}

/* Round-robin: advance to next runnable slot */
static vcpu_t *find_next_rr(u32 *dur_out)
{
    u32 start = _cur;
    do {
        _cur = (_cur + 1) % _nslots;
        vcpu_t *vc = slot_vcpu(_cur);
        if (vc) { *dur_out = _slots[_cur].duration_us; return vc; }
    } while (_cur != start);
    return NULL;
}

/* Find next best-effort (non-RT) runnable slot */
static vcpu_t *find_next_be(u32 *dur_out)
{
    u32 start = _cur;
    do {
        _cur = (_cur + 1) % _nslots;
        if (slot_is_rt(_cur)) continue;
        vcpu_t *vc = slot_vcpu(_cur);
        if (vc) { *dur_out = _slots[_cur].duration_us; return vc; }
    } while (_cur != start);
    return NULL;
}

/*
 * do_switch — perform context switch with logging and stats update.
 *
 * reason: "timer" or "wfi" — printed in the context switch log line.
 */
static void do_switch(vcpu_t *prev, vcpu_t *next,
                      u32 next_dur, bool is_preempt)
{
    u32 prev_vm_id = (prev && prev->vm) ? prev->vm->id : 0;
    u32 next_vm_id = (next && next->vm) ? next->vm->id : 0;

    /*
     * Phase 3: context switch log.
     * Format: CTX[N]: <from_vm>→<to_vm> (<slice>ms, <reason>)
     * Only log when the VM actually changes to avoid noise.
     */
    if (prev_vm_id != next_vm_id) {
        /* Use only format specs supported by the minimal log printf:
         * %u, %d, %s, %lx — no %lu/%llu/%-Ns */
        const char *prev_name = (prev && prev->vm) ? prev->vm->name : "idle";
        const char *next_name = (next && next->vm) ? next->vm->name : "idle";
        const char *reason    = is_preempt ? "timer" : "wfi";
        u32 sw = (u32)(g_switch_count & 0xFFFFFFFFULL);
        u32 ms = next_dur / 1000u;

        LOG_INFO("CTX[%u]: %s -> %s (%ums, %s)",
                 sw, prev_name, next_name, ms, reason);

        /* Audit fix #4: periodic IDS monitor (heartbeat + trust-downgrade scan).
         * Header targets ~10s; switches are 0.5-2s, so poll every 8th VM change.
         * Single-core assumption: ids_poll reads g_ids/g_trust with no lock;
         * an SMP port (CPU_ON) must add locking here. */
        static u32 ids_poll_ctr = 0;
        if ((++ids_poll_ctr % 8u) == 0u)
            ids_poll();
    }

    /* Phase 3: update CPU accounting */
    sched_stats_on_switch(prev_vm_id, next_vm_id,
                          next_dur, is_preempt);

    /* Perform the actual hardware context switch */
    if (prev != next) vcpu_do_switch(prev, next);
}

/* ── Public scheduler API ── */

err_t sched_init(void)
{
    memset(_slots, 0, sizeof(_slots));
    _nslots = 0;
    _cur    = 0;

    /* Phase 3: initialise CPU accounting */
    sched_stats_init();

    LOG_INFO("Scheduler init start");
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = g_hyp.vms[i];
        if (!vm) continue;
        LOG_INFO("Adding VM %d '%s' (realtime=%d) with %d vCPUs",
                 vm->id, vm->name, (int)vm->sched.realtime, vm->num_vcpus);
        for (u32 v = 0; v < vm->num_vcpus; v++)
            sched_add_slot(vm->id, v, vm->sched.time_slice_us);
    }
    LOG_INFO("Scheduler ready: %d slots, major-frame=%d us",
             _nslots, SCHED_MAJOR_FRAME_US);
    return E_OK;
}

void sched_add_slot(u32 vm_id, u32 vcpu_id, u32 dur_us)
{
    if (_nslots >= MAX_SLOTS) return;
    _slots[_nslots].vm_id       = vm_id;
    _slots[_nslots].vcpu_id     = vcpu_id;
    _slots[_nslots].duration_us = dur_us ? dur_us : 1000;

    /* Phase 3: register initial slice in stats */
    sched_stats_set_slice(vm_id, dur_us ? dur_us : 1000);

    _nslots++;
}

/*
 * sched_set_slice — Phase 3: change a VM's time slice at runtime.
 * Takes effect on the next timer tick for that VM's slot.
 */
err_t sched_set_slice(u32 vm_id, u32 dur_us)
{
    if (dur_us < 100) dur_us = 100;         /* 100us minimum */
    if (dur_us > 1000000) dur_us = 1000000; /* 1s maximum    */

    bool found = false;
    for (u32 i = 0; i < _nslots; i++) {
        if (_slots[i].vm_id == vm_id) {
            _slots[i].duration_us = dur_us;
            found = true;
        }
    }
    if (!found) return E_NOTFOUND;

    sched_stats_set_slice(vm_id, dur_us);
    LOG_INFO("Sched: VM%u slice updated to %uus", vm_id, dur_us);
    return E_OK;
}

err_t sched_get_stats(u32 vm_id, vm_stats_t *out)
{
    return sched_stats_get(vm_id, out);
}

/*
 * sched_on_timer — called on every CNTHP timer IRQ (preemptive switch).
 * Advances round-robin. Logs the switch with "timer" reason.
 */
void sched_on_timer(void)
{
    if (!_nslots) { timer_set_us(SCHED_MAJOR_FRAME_US); return; }

    vcpu_t *prev     = g_current_vcpu[0];
    vcpu_t *next     = NULL;
    u32     next_dur = SCHED_MAJOR_FRAME_US;

    next = find_next_rr(&next_dur);
    if (!next) { timer_set_us(SCHED_MAJOR_FRAME_US); return; }

    /* Phase 3: log + stats (is_preempt=true for timer IRQ) */
    do_switch(prev, next, next_dur, true);

    gate_uart(next);
    timer_set_us(next_dur);
}

/*
 * sched_on_wfi — called when EC_WFI fires (voluntary yield via WFI).
 *
 * Key fix: we use find_next_rr() but SKIP the current VM (the one that
 * just WFI'd). This means:
 *   - Linux WFI → picks RTOS or Android (in round-robin order)
 *   - RTOS WFI  → picks Linux or Android (RTOS gets its turn back next cycle)
 *   - Android WFI → picks RTOS or Linux
 *
 * This ensures RTOS always gets its 1ms RT slot even when Linux/Android
 * are idle. The previous find_next_be() approach skipped RTOS entirely
 * when both Linux and Android were WFI-ing.
 */
void sched_on_wfi(void)
{
    vcpu_t *prev = g_current_vcpu[0];

    /* Advance ELR past the WFI instruction */
    if (prev) {
        prev->regs.elr_el2 += 4;

        /* Phase 3: record voluntary WFI yield */
        if (prev->vm) sched_stats_on_wfi(prev->vm->id);
    }

    /* Find next runnable slot, skipping the VM that just WFI'd */
    u32 start = _cur;
    vcpu_t *next     = NULL;
    u32     next_dur = SCHED_MAJOR_FRAME_US;
    u32     prev_vm_id = (prev && prev->vm) ? prev->vm->id : 0;

    do {
        _cur = (_cur + 1) % _nslots;
        /* Skip the VM that just WFI'd to avoid immediate re-schedule */
        if (_slots[_cur].vm_id == prev_vm_id) continue;
        vcpu_t *vc = slot_vcpu(_cur);
        if (vc) { next = vc; next_dur = _slots[_cur].duration_us; break; }
    } while (_cur != start);

    /* Fallback: full round-robin if all others are stopped */
    if (!next) {
        next = find_next_rr(&next_dur);
    }

    if (!next) { timer_set_us(SCHED_MAJOR_FRAME_US); return; }

    /* Phase 3: log + stats (is_preempt=false for voluntary WFI) */
    do_switch(prev, next, next_dur, false);

    gate_uart(next);
    timer_set_us(next_dur);
}

void sched_run(void)
{
    if (!_nslots || !g_hyp.num_vms) {
        LOG_WARN("No VMs — idle loop");
        while (1) WFI();
    }

    LOG_INFO("Starting VMs");
    for (u32 i = 0; i < g_hyp.num_vms; i++) {
        vm_t *vm = g_hyp.vms[i];
        if (vm && vm->state == VM_READY) vm_start(vm);
    }

    /* Pick first RT slot, fall back to any slot */
    vcpu_t *first     = NULL;
    u32     first_dur = SCHED_MAJOR_FRAME_US;

    for (u32 i = 0; i < _nslots && !first; i++) {
        if (!slot_is_rt(i)) continue;
        vm_t *vm = vm_by_id(_slots[i].vm_id);
        if (!vm || vm->state != VM_RUNNING) continue;
        if (_slots[i].vcpu_id >= vm->num_vcpus) continue;
        first     = vm->vcpus[_slots[i].vcpu_id];
        first_dur = _slots[i].duration_us;
        _cur      = i;
    }
    for (u32 i = 0; i < _nslots && !first; i++) {
        vm_t *vm = vm_by_id(_slots[i].vm_id);
        if (!vm || vm->state != VM_RUNNING) continue;
        if (_slots[i].vcpu_id >= vm->num_vcpus) continue;
        first     = vm->vcpus[_slots[i].vcpu_id];
        first_dur = _slots[i].duration_us;
        _cur      = i;
    }

    if (!first) hyp_panic("No runnable vCPU found");

    g_current_vcpu[0] = first;
    first->state      = VCPU_RUNNING;

    LOG_INFO("----------------------------------");
    LOG_INFO("Launching first guest");
    LOG_INFO("VM ID        : %d",    first->vm->id);
    LOG_INFO("VM Name      : %s",    first->vm->name);
    LOG_INFO("vCPU ID      : %d",    first->vcpu_id);
    LOG_INFO("Entry PC     : 0x%lx", first->regs.elr_el2);
    LOG_INFO("SPSR_EL2     : 0x%lx", first->regs.spsr_el2);
    LOG_INFO("DTB arg (x0) : 0x%lx", first->regs.x[0]);
    LOG_INFO("VTTBR_EL2    : 0x%lx", first->vttbr_el2);
    LOG_INFO("----------------------------------");

    vcpu_restore_sysregs(&first->sysregs);

    u64 mair = (0x00ULL << 0) | (0xFFULL << 8);
    WRITE_SYSREG(mair_el2, mair);
    ISB();

    __asm__ volatile("msr vtcr_el2,  %0\nisb\n" :: "r"(VTCR_DEFAULT)     : "memory");
    __asm__ volatile("msr vttbr_el2, %0\nisb\n" :: "r"(first->vttbr_el2) : "memory");
    __asm__ volatile("msr hcr_el2,   %0\nisb\n" :: "r"((u64)HCR_GUEST)   : "memory");

    LOG_INFO("Executing ERET -> Guest");

    /* Set timer AFTER all log output so first slice is a full slice */
    timer_set_us(first_dur);

    __asm__ volatile(
        "msr elr_el2,  %0\n"
        "msr spsr_el2, %1\n"
        "mov x1, xzr\n"
        "mov x2, xzr\n"
        "mov x3, xzr\n"
        "ldp x0, x4, [%2, #(0*8)]\n"
        "eret\n"
        :: "r"(first->regs.elr_el2),
           "r"(first->regs.spsr_el2),
           "r"(&first->regs.x[0])
        : "x0", "x1", "x2", "x3");

    __builtin_unreachable();
}

void sched_secondary_run(u32 cpu_id) { UNUSED(cpu_id); while (1) WFI(); }
