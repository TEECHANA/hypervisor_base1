/*
 * hvc_abi.h — Tessolve Hypervisor Hypercall ABI
 *
 * Calling convention:  HVC #0
 *   x0 = hypercall ID (in)  /  return code (out)
 *   x1-x7 = arguments
 */
#ifndef HYP_HVC_ABI_H
#define HYP_HVC_ABI_H
#include "../../include/types.h"

/* ── VM management ── */
#define HVC_VM_GET_ID       0x0001u  /* → x0=current vm_id               */
#define HVC_VM_QUERY_STATE  0x0002u  /* x1=vm_id → x0=vm_state_t         */
#define HVC_VM_STOP         0x0003u  /* x1=vm_id → x0=err                */
#define HVC_VM_SUSPEND      0x0004u  /* x1=vm_id → x0=err                */
#define HVC_VM_RESUME       0x0005u  /* x1=vm_id → x0=err                */

/* ── Shared memory ── */
#define HVC_SHMEM_MAP       0x0010u  /* x1=src_vm x2=dst_vm x3=ipa x4=sz */
#define HVC_SHMEM_UNMAP     0x0011u  /* x1=ipa x2=sz                      */

/* ── IPC ── */
#define HVC_IPC_SEND        0x0020u  /* x1=dst_vm x2=buf_ipa x3=len       */
#define HVC_IPC_RECV        0x0021u  /* x1=buf_ipa x2=max_len → x0=len   */
#define HVC_IPC_NOTIFY      0x0022u  /* x1=dst_vm                         */

/* ── Debug ── */
#define HVC_LOG_WRITE       0x00F0u  /* x1=buf_ipa x2=len                 */
#define HVC_PERF_QUERY      0x00F1u  /* → x0=cpu_permille                 */

#endif /* HYP_HVC_ABI_H */
