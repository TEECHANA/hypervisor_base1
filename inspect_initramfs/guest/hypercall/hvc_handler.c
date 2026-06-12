/*
 * hvc_handler.c — Hypercall dispatch
 * Called from hyp_sync_handler when ESR_EL2.EC == EC_HVC64.
 * regs points to the guest_regs_t on the EL2 stack.
 */
#include "hvc_abi.h"
#include "../../include/error.h"
#include "../../lib/log/log.h"
#include "../../core/vm/vm.h"

extern err_t shmem_map  (u32 src, u32 dst, ipa_t ipa, u64 sz);
extern err_t shmem_unmap(ipa_t ipa, u64 sz);
extern err_t ipc_send   (u32 dst, ipa_t buf, u64 len);
extern err_t ipc_recv   (ipa_t buf, u64 max, u64 *out_len);
extern err_t ipc_notify (u32 dst);

/* Implemented in arch/arm64/psci.c */
extern void psci_handler(u64 *regs);

static void hvc_log(u64 buf_ipa, u64 len)
{
    if(len>256) len=256;
    LOG_DEBUG("[GUEST] buf_ipa=0x%lx len=%ld", buf_ipa, len);
}

void hvc_dispatch(void *regs)
{
    u64 *x = (u64*)regs;
    LOG_INFO("HVC: func=0x%lx", x[0]);   // ADD THIS
    u64  func = x[0];

    /* ── PSCI routing ──────────────────────────────────────────────────
     * ARM SMC CC: bits[31:24] encodes call type, bits[23:16] = owner.
     * Owner 0x04 = Standard Secure Service = PSCI.
     * Covers 32-bit (0x84xxxxxx) and 64-bit (0xC4xxxxxx) function IDs. */
    u32 owner = (u32)((func >> 24) & 0x3F);
    if (owner <= 0x06) {
        psci_handler(x);
        return;
    }

    /* ── Hypervisor-specific hypercalls ─────────────────────────────── */
    u32 id  = (u32)func;
    err_t r = E_OK;

    switch(id){
    case HVC_VM_GET_ID:
        x[0] = 1;
        break;
    case HVC_VM_QUERY_STATE: {
        vm_t *vm = vm_by_id((u32)x[1]);
        x[0] = vm ? (u64)vm->state : (u64)(s64)E_NOTFOUND;
        break;
    }
    case HVC_VM_STOP: {
        vm_t *vm = vm_by_id((u32)x[1]);
        r = vm ? vm_stop(vm) : E_NOTFOUND;
        x[0] = (u64)(s64)r; break;
    }
    case HVC_VM_SUSPEND: {
        vm_t *vm = vm_by_id((u32)x[1]);
        r = vm ? vm_suspend(vm) : E_NOTFOUND;
        x[0] = (u64)(s64)r; break;
    }
    case HVC_VM_RESUME: {
        vm_t *vm = vm_by_id((u32)x[1]);
        r = vm ? vm_resume(vm) : E_NOTFOUND;
        x[0] = (u64)(s64)r; break;
    }
    case HVC_SHMEM_MAP:
        x[0] = (u64)(s64)shmem_map((u32)x[1],(u32)x[2],x[3],x[4]); break;
    case HVC_SHMEM_UNMAP:
        x[0] = (u64)(s64)shmem_unmap(x[1],x[2]); break;
    case HVC_IPC_SEND:
        x[0] = (u64)(s64)ipc_send((u32)x[1],x[2],x[3]); break;
    case HVC_IPC_RECV: {
        u64 len=0;
        r = ipc_recv(x[1],x[2],&len);
        x[0] = OK(r) ? len : (u64)(s64)r; break;
    }
    case HVC_IPC_NOTIFY:
        x[0] = (u64)(s64)ipc_notify((u32)x[1]); break;
    case HVC_LOG_WRITE:
        hvc_log(x[1],x[2]); x[0]=E_OK; break;
    default:
        LOG_WARN("HVC: unknown id=0x%lx", func);
        x[0] = (u64)(s64)E_UNSUPPORTED;
    }
}
