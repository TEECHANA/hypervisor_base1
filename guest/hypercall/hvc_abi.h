/*
 * hvc_abi.h — Tessolve Hypervisor Hypercall ABI (Phase 4A)
 *
 * Phase 4A addition:
 *   HVC_PMU_QUERY (0x0040) — query calling vCPU's PMU counters
 *     → x0 = cycles_lo (u32)
 *     → x1 = cycles_hi (u32)
 *     → x2 = instructions
 *     → x3 = cache_misses
 */

#ifndef HYP_HVC_ABI_H
#define HYP_HVC_ABI_H

#include "../../include/types.h"

/* ── VM management ── */
#define HVC_VM_GET_ID       0x0001u
#define HVC_VM_QUERY_STATE  0x0002u
#define HVC_VM_STOP         0x0003u
#define HVC_VM_SUSPEND      0x0004u
#define HVC_VM_RESUME       0x0005u

/* ── Shared memory ── */
#define HVC_SHMEM_MAP       0x0010u
#define HVC_SHMEM_UNMAP     0x0011u

/* ── IPC ── */
#define HVC_IPC_SEND        0x0020u
#define HVC_IPC_RECV        0x0021u
#define HVC_IPC_NOTIFY      0x0022u

/* ── Scheduler ── */
#define HVC_SCHED_SET_SLICE 0x0030u
#define HVC_SCHED_GET_STATS 0x0031u
#define HVC_SCHED_YIELD     0x0032u

/* ── PMU (Phase 4A) ── */
#define HVC_PMU_QUERY       0x0040u  /* → x0=cycles_lo x1=cycles_hi x2=instrs x3=misses */
#define HVC_PMU_RESET       0x0041u  /* reset calling vCPU's accumulated counters → E_OK */

/* ── Debug ── */
#define HVC_LOG_WRITE       0x00F0u
#define HVC_PERF_QUERY      0x00F1u

#endif /* HYP_HVC_ABI_H */
