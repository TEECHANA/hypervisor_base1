/*
 * ipc.c — Single-slot message queue per destination VM
 * (Expandable to a ring buffer in Phase 3)
 */
#include "ipc.h"
#include "../../include/config.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

#define IPC_MSG_MAX 256
#define IPC_DEPTH   8

typedef struct { u8 data[IPC_MSG_MAX]; u32 len; u32 src; } msg_t;
typedef struct { msg_t msgs[IPC_DEPTH]; u32 head,tail,cnt; } queue_t;

static queue_t _q[MAX_VMS];

err_t ipc_init(void){
    memset(_q,0,sizeof(_q)); return E_OK;}

err_t ipc_send(u32 dst, ipa_t buf_ipa, u64 len)
{
    if(dst>=MAX_VMS||len>IPC_MSG_MAX) return E_INVAL;
    queue_t *q=&_q[dst];
    if(q->cnt>=IPC_DEPTH) return E_BUSY;
    msg_t *m=&q->msgs[q->tail];
    m->len=(u32)len;
    /* TODO Phase 3: copy from guest IPA using s2 walk */
    UNUSED(buf_ipa);
    q->tail=(q->tail+1)%IPC_DEPTH; q->cnt++;
    LOG_DEBUG("IPC: %ld B -> VM%d", len, dst);
    return E_OK;
}

err_t ipc_recv(ipa_t buf_ipa, u64 max_len, u64 *out_len)
{
    UNUSED(buf_ipa); UNUSED(max_len);
    /* TODO Phase 3: determine caller's vm_id */
    *out_len=0; return E_OK;
}

err_t ipc_notify(u32 dst)
{
    /* TODO Phase 3: inject virtual IRQ to dst */
    UNUSED(dst); return E_OK;
}
