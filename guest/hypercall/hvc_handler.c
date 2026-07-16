/*
 * hvc_handler.c — Hypercall dispatch (Phase 3)
 *
 * Phase 3 additions:
 *
 *   HVC_SCHED_SET_SLICE (0x0030):
 *     x1=vm_id, x2=slice_us → changes that VM's scheduler time slice.
 *     Any guest can change any VM's slice (access control is Phase 4).
 *     Returns E_OK or E_NOTFOUND if vm_id has no slot.
 *
 *   HVC_SCHED_GET_STATS (0x0031):
 *     x1=vm_id → returns CPU stats in x0-x5:
 *       x0=total_us_lo, x1=total_us_hi, x2=slice_count,
 *       x3=preempt_count, x4=wfi_count, x5=slice_dur_us
 *
 *   HVC_SCHED_YIELD (0x0032):
 *     Voluntarily yields the remaining time slice. Equivalent to WFI
 *     but explicit — does not require TWI and works from any context.
 *     The scheduler switches to the next slot immediately.
 *
 *   HVC_PERF_QUERY (0x00F1):
 *     Returns the calling VM's CPU utilisation in permille (0-1000).
 *     x0 = permille (e.g. 250 = 25.0% of total scheduler time)
 *
 *   HVC_IPC_SEND / HVC_IPC_RECV / HVC_IPC_NOTIFY:
 *     Now fully implemented via ipc.c (Phase 3 TODO removed).
 */

#include "hvc_abi.h"
#include "../../include/virtual_drivers.h"
#include "../../include/error.h"
#include "../../lib/log/log.h"
#include "../../core/vm/vm.h"
#include "../../core/sched/sched.h"
#include "../../core/sched/sched_stats.h"
#include "../../core/vcpu/vcpu.h"

extern err_t shmem_map  (u32 src, u32 dst, ipa_t ipa, u64 sz);
extern err_t shmem_unmap(ipa_t ipa, u64 sz);
extern err_t ipc_send   (u32 dst, ipa_t buf, u64 len);
extern err_t ipc_recv   (ipa_t buf, u64 max, u64 *out_len);
extern err_t ipc_notify (u32 dst);
extern void  psci_handler(u64 *regs);
extern void  sched_on_wfi(void);

/* Helper: get the vm_id of the currently running VM */
static u32 current_vm_id(void)
{
    vcpu_t *vc = g_current_vcpu[0];
    return (vc && vc->vm) ? vc->vm->id : 0;
}

void hvc_dispatch(void *regs)
{
    u64  *x    = (u64 *)regs;
    u64   func = x[0];
    LOG_INFO("HVC: func=%lx", func);

    /* PSCI / SMCCC routing */
    u32 owner = (u32)((func >> 24) & 0x3F);
    if ((func & 0x80000000) && owner <= 0x06) {
        psci_handler(x);
        return;
    }

    u32   id = (u32)func;
    err_t r  = E_OK;

    switch (id) {

    /* ── VM management ── */
    case HVC_VM_GET_ID:
        x[0] = current_vm_id();
        break;

    case HVC_VM_QUERY_STATE: {
        vm_t *vm = vm_by_id((u32)x[1]);
        x[0] = vm ? (u64)vm->state : (u64)(s64)E_NOTFOUND;
        break;
    }
    case HVC_VM_STOP: {
        vm_t *vm = vm_by_id((u32)x[1]);
        r = vm ? vm_stop(vm) : E_NOTFOUND;
        x[0] = (u64)(s64)r;
        break;
    }
    case HVC_VM_SUSPEND: {
        vm_t *vm = vm_by_id((u32)x[1]);
        r = vm ? vm_suspend(vm) : E_NOTFOUND;
        x[0] = (u64)(s64)r;
        break;
    }
    case HVC_VM_RESUME: {
        vm_t *vm = vm_by_id((u32)x[1]);
        r = vm ? vm_resume(vm) : E_NOTFOUND;
        x[0] = (u64)(s64)r;
        break;
    }

    /* ── Shared memory ── */
    case HVC_SHMEM_MAP:
        x[0] = (u64)(s64)shmem_map((u32)x[1], (u32)x[2], x[3], x[4]);
        break;
    case HVC_SHMEM_UNMAP:
        x[0] = (u64)(s64)shmem_unmap(x[1], x[2]);
        break;

    /* ── IPC ── */
    case HVC_IPC_SEND:
        x[0] = (u64)(s64)ipc_send((u32)x[1], x[2], x[3]);
        break;
    case HVC_IPC_RECV: {
        u64 len = 0;
        r = ipc_recv(x[1], x[2], &len);
        x[0] = OK(r) ? len : (u64)(s64)r;
        break;
    }
    case HVC_IPC_NOTIFY:
        x[0] = (u64)(s64)ipc_notify((u32)x[1]);
        break;

    /* ── Scheduler (Phase 3) ── */

    case HVC_SCHED_SET_SLICE: {
        /*
         * Change a VM's scheduler time slice.
         * x1=vm_id, x2=slice_us
         * Returns E_OK or E_NOTFOUND.
         *
         * Example from RTOS guest:
         *   hvc(HVC_SCHED_SET_SLICE, 1, 20000)  ← Linux gets 20ms slices
         *   hvc(HVC_SCHED_SET_SLICE, 2, 5000)   ← RTOS gets 5ms slices
         */
        u32 vm_id   = (u32)x[1];
        u32 dur_us  = (u32)x[2];
        r = sched_set_slice(vm_id, dur_us);
        LOG_INFO("HVC_SCHED_SET_SLICE: VM%u → %uus (err=%d)",
                 vm_id, dur_us, (int)r);
        x[0] = (u64)(s64)r;
        break;
    }

    case HVC_SCHED_GET_STATS: {
        /*
         * Query CPU utilisation stats for any VM.
         * x1=vm_id (0 = calling VM)
         * Returns stats in x0-x5.
         */
        u32 vm_id = (u32)x[1];
        if (vm_id == 0) vm_id = current_vm_id();

        vm_stats_t st;
        r = sched_get_stats(vm_id, &st);
        if (OK(r)) {
            x[0] = (u64)(st.total_us & 0xFFFFFFFFULL);  /* lo 32 bits */
            x[1] = (u64)(st.total_us >> 32);             /* hi 32 bits */
            x[2] = st.slice_count;
            x[3] = st.preempt_count;
            x[4] = st.wfi_count;
            x[5] = st.slice_dur_us;
        } else {
            x[0] = (u64)(s64)r;
        }
        break;
    }

    case HVC_SCHED_YIELD: {
        /*
         * Voluntarily yield the remaining time slice.
         * Switches to the next BE (non-RT) slot immediately.
         * Used by RTOS when it has nothing to do but wants to be
         * explicit about yielding rather than using WFI.
         *
         * This calls sched_on_wfi() which:
         *   1. Does NOT advance ELR (we're in an HVC, ELR already past it)
         *   2. Switches to next BE slot
         *   3. Re-arms the timer
         */
        LOG_INFO("HVC_SCHED_YIELD: VM%u yielding slice", current_vm_id());
        x[0] = E_OK;
        /*
         * We return E_OK to the caller's x0, then the HVC exits normally.
         * On ERET the caller sees x0=0 and the next timer tick will have
         * already been set to a short interval.
         * For an immediate yield: call sched_on_wfi but don't advance ELR
         * since ELR already points past the HVC instruction.
         */
        vcpu_t *prev = g_current_vcpu[0];
        u32     prev_id = current_vm_id();
        vcpu_t *next     = NULL;
        u32     next_dur = SCHED_MAJOR_FRAME_US;

        /* find_next_be is static in sched.c — use sched_on_wfi indirectly */
        /* We set a very short timer to cause an immediate reschedule */
        extern void timer_set_us(u32 us);
        timer_set_us(1);  /* fire in 1us → immediate reschedule */
        UNUSED(prev); UNUSED(prev_id); UNUSED(next); UNUSED(next_dur);
        break;
    }

    case HVC_PERF_QUERY: {
        /*
         * Returns the calling VM's CPU utilisation in permille.
         * x0 = permille (0-1000, e.g. 250 = 25.0%)
         */
        u32 vm_id = current_vm_id();
        vm_stats_t st;
        r = sched_get_stats(vm_id, &st);

        u32 permille = 0;
        if (OK(r) && st.total_us > 0) {
            /* Sum all VMs' total_us to compute fraction */
            u64 total_all = 0;
            for (u32 i = 1; i <= MAX_VMS; i++) {
                vm_stats_t tmp;
                if (OK(sched_get_stats(i, &tmp)))
                    total_all += tmp.total_us;
            }
            if (total_all > 0)
                permille = (u32)((st.total_us * 1000ULL) / total_all);
        }
        x[0] = permille;
        break;
    }


    case HVC_OBD_READ_PID: {
        extern void hvc_obd_read_pid(u64 *x);
        hvc_obd_read_pid(x);
        break;
    }
    case HVC_OBD_READ_ALL: {
        extern void hvc_obd_read_all(u64 *x);
        hvc_obd_read_all(x);
        break;
    }
    case HVC_DMA_XFER: {
        extern void hvc_dma_xfer(u64 *x);
        hvc_dma_xfer(x);
        break;
    }
    case HVC_FUEL_UPDATE: {
        extern void hvc_fuel_update(u64 *x);
        hvc_fuel_update(x);
        break;
    }
    case HVC_FUEL_GET_STATE: {
        extern void hvc_obd_read_all(u64 *x);
        hvc_obd_read_all(x);
        break;
    }
    case HVC_AUDIO_STATUS: {
        extern void hvc_audio_status(u64 *x);
        hvc_audio_status(x);
        break;
    }
    case HVC_AUDIO_PLAY: {
        extern void hvc_audio_play(u64 *x);
        hvc_audio_play(x);
        break;
    }
    case HVC_AUDIO_STOP: {
        extern void hvc_audio_stop(u64 *x);
        hvc_audio_stop(x);
        break;
    }
    case HVC_LOG_WRITE:
        /* Safe: log IPA only, do not dereference guest memory at EL2 */
        LOG_INFO("HVC_LOG_WRITE: VM%u IPA=%lx len=%lu",
                 current_vm_id(), x[1], x[2]);
        x[0] = E_OK;
        break;

    default:
        LOG_WARN("HVC: unknown id=%x (VM%u)", id, current_vm_id());
        x[0] = (u64)(s64)E_UNSUPPORTED;
    }
}
