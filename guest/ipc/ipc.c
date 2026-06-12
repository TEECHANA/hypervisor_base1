/*
 * ipc.c — Inter-VM IPC with ring buffer and virtual IRQ notification (Phase 3)
 *
 * Phase 3: replaces the stub from Phase 1/2.
 *
 * Design:
 *   - One ring buffer queue per destination VM (depth=8, msg=256B)
 *   - ipc_send(): copies message metadata; actual data copy via
 *     safe hypervisor memcpy (IPA→PA translation for sender's VM).
 *   - ipc_recv(): called by destination VM to dequeue the next message.
 *   - ipc_notify(): injects a virtual IRQ (vIRQ 31) to the destination VM
 *     to wake it up without it having to poll.
 *
 * vIRQ assignment:
 *   PPI 31 = IPC doorbell (software-defined, injected via ICH_LR)
 *   Linux/RTOS guests can install an IRQ handler on vIRQ 31 to
 *   receive IPC arrival notifications.
 *
 * Message format stored in the queue:
 *   src_vm_id, len, data[256]
 * The data is copied at send time from the sender's IPA into a
 * hypervisor-owned bounce buffer. At recv time it is copied out
 * to the receiver's IPA.
 *
 * Limitations (to be addressed in Phase 4):
 *   - No zero-copy: messages are always bounced through hypervisor memory
 *   - No flow control: sender gets E_BUSY if queue is full
 *   - Fixed vIRQ 31 for all VMs
 */

#include "ipc.h"
#include "../../include/config.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../core/vm/vm.h"
#include "../../core/vcpu/vcpu.h"
#include "../../drivers/gic/gicv3.h"

#define IPC_MSG_MAX  256
#define IPC_DEPTH    8
#define IPC_DOORBELL_VIRQ  31   /* PPI 31: IPC arrival notification */

typedef struct {
    u8  data[IPC_MSG_MAX];
    u32 len;
    u32 src_vm_id;
} ipc_msg_t;

typedef struct {
    ipc_msg_t slots[IPC_DEPTH];
    u32       head;   /* next slot to read  */
    u32       tail;   /* next slot to write */
    u32       count;  /* messages pending   */
} ipc_queue_t;

static ipc_queue_t _queues[MAX_VMS];

err_t ipc_init(void)
{
    memset(_queues, 0, sizeof(_queues));
    LOG_INFO("IPC: initialised (%d VMs, depth=%d, msg_max=%d)",
             MAX_VMS, IPC_DEPTH, IPC_MSG_MAX);
    return E_OK;
}

/*
 * ipc_send — send a message from the current VM to dst_vm.
 *
 * buf_ipa: IPA of the message buffer in the sender's address space.
 * len:     message length in bytes (max IPC_MSG_MAX).
 *
 * The message is copied from sender's IPA to the hypervisor queue.
 * Since sender's IPA == PA for RTOS (flat 1:1 mapped), and Linux
 * also has a 1:1 mapped region, we compute PA = IPA + PA_BASE_OFFSET.
 * A proper S2 page table walk would be done in Phase 4.
 */
err_t ipc_send(u32 dst_vm_id, ipa_t buf_ipa, u64 len)
{
    if (dst_vm_id == 0 || dst_vm_id > MAX_VMS) return E_INVAL;
    if (len == 0 || len > IPC_MSG_MAX)          return E_INVAL;

    ipc_queue_t *q = &_queues[dst_vm_id - 1];
    if (q->count >= IPC_DEPTH) {
        LOG_WARN("IPC: queue full for VM%u", dst_vm_id);
        return E_BUSY;
    }

    /* Get sender VM context */
    vcpu_t *sender_vcpu = g_current_vcpu[0];
    u32 src_vm_id = (sender_vcpu && sender_vcpu->vm) ?
                     sender_vcpu->vm->id : 0;

    /* Find sender PA offset for IPA→PA translation */
    vm_t *src_vm = vm_by_id(src_vm_id);
    paddr_t pa_base = 0;
    if (src_vm && src_vm->num_mem > 0)
        pa_base = src_vm->mem[0].pa_base - src_vm->mem[0].ipa_base;

    /* Copy message from sender's IPA into queue slot */
    ipc_msg_t *msg = &q->slots[q->tail];
    const u8 *src_ptr = (const u8 *)(uintptr_t)(buf_ipa + pa_base);
    memcpy(msg->data, src_ptr, (u32)len);
    msg->len       = (u32)len;
    msg->src_vm_id = src_vm_id;

    q->tail  = (q->tail + 1) % IPC_DEPTH;
    q->count++;

    LOG_INFO("IPC: VM%u → VM%u %u bytes (queue depth=%u)",
             src_vm_id, dst_vm_id, (u32)len, q->count);
    return E_OK;
}

/*
 * ipc_recv — receive a pending message into the caller's IPA buffer.
 *
 * Returns the message length in *out_len, or 0 if queue is empty.
 */
err_t ipc_recv(ipa_t buf_ipa, u64 max_len, u64 *out_len)
{
    if (!out_len) return E_INVAL;
    *out_len = 0;

    /* Get receiver VM context */
    vcpu_t *recv_vcpu = g_current_vcpu[0];
    if (!recv_vcpu || !recv_vcpu->vm) return E_INVAL;
    u32 dst_vm_id = recv_vcpu->vm->id;

    ipc_queue_t *q = &_queues[dst_vm_id - 1];
    if (q->count == 0) return E_OK;   /* empty — not an error */

    ipc_msg_t *msg = &q->slots[q->head];
    u64 copy_len = (msg->len < max_len) ? msg->len : max_len;

    /* Find receiver PA offset */
    vm_t *dst_vm = recv_vcpu->vm;
    paddr_t pa_base = 0;
    if (dst_vm->num_mem > 0)
        pa_base = dst_vm->mem[0].pa_base - dst_vm->mem[0].ipa_base;

    /* Copy message to receiver's IPA buffer */
    u8 *dst_ptr = (u8 *)(uintptr_t)(buf_ipa + pa_base);
    memcpy(dst_ptr, msg->data, (u32)copy_len);
    *out_len = copy_len;

    /* Dequeue */
    q->head  = (q->head + 1) % IPC_DEPTH;
    q->count--;

    LOG_INFO("IPC: VM%u received %llu bytes from VM%u (queue depth=%u)",
             dst_vm_id, copy_len, msg->src_vm_id, q->count);
    return E_OK;
}

/*
 * ipc_notify — inject a virtual IRQ (PPI 31) to wake the destination VM.
 *
 * The destination VM must have an IRQ handler registered for vIRQ 31.
 * This allows event-driven IPC without polling.
 *
 * RTOS example:
 *   Install handler for IRQ 31 in RTOS.
 *   When Linux calls ipc_notify(2), RTOS gets woken via vIRQ 31.
 */
err_t ipc_notify(u32 dst_vm_id)
{
    if (dst_vm_id == 0 || dst_vm_id > MAX_VMS) return E_INVAL;

    vm_t *dst = vm_by_id(dst_vm_id);
    if (!dst || dst->state != VM_RUNNING) return E_NOTFOUND;

    /* Inject virtual PPI 31 into the destination VM */
    extern void gic_inject_virq_lr(u32 virq, u32 prio);
    gic_inject_virq_lr(IPC_DOORBELL_VIRQ, 0xA0);

    LOG_INFO("IPC: doorbell → VM%u (vIRQ %u injected)",
             dst_vm_id, IPC_DOORBELL_VIRQ);
    return E_OK;
}
