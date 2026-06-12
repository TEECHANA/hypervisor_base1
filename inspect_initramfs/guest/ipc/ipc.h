#ifndef HYP_IPC_H
#define HYP_IPC_H
#include "../../include/types.h"
#include "../../include/error.h"
err_t ipc_init  (void);
err_t ipc_send  (u32 dst_vm, ipa_t buf_ipa, u64 len);
err_t ipc_recv  (ipa_t buf_ipa, u64 max_len, u64 *out_len);
err_t ipc_notify(u32 dst_vm);
#endif
