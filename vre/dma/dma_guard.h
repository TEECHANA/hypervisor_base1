/*
 * dma_guard.h — DMA address and protection manager (Phase 2 §2.2.2)
 *
 * Validates DMA target addresses against a VM's Stage-2 page tables
 * before allowing a peripheral to access physical memory.
 *
 * On real hardware this is enforced by the SMMU; this module provides
 * the software validation layer that:
 *   (a) the hypervisor calls before programming a DMA descriptor, and
 *   (b) the SMMU fault handler calls to log policy violations.
 *
 * The key invariant: a DMA transaction targeting PA X is safe for VM N
 * if and only if IPA X is mapped in VM N's Stage-2 page tables.
 * (For VMs whose IPA==PA for their DMA region this is a direct check.)
 */

#ifndef HYP_DMA_GUARD_H
#define HYP_DMA_GUARD_H

#include "../../include/types.h"
#include "../../include/error.h"

struct vm;

/*
 * dma_guard_check — validate a DMA buffer [pa, pa+size) for vm.
 *
 * Returns E_OK    if the entire range is mapped in vm's Stage-2 tables
 *         E_PERM  if any page in the range is not mapped (DMA violation)
 *         E_INVAL if arguments are invalid
 *
 * The caller must NOT initiate the DMA transaction if this returns != E_OK.
 */
err_t dma_guard_check(struct vm *vm, paddr_t pa, u64 size);

/*
 * dma_guard_check_ipa — same as dma_guard_check but takes an IPA.
 * Use when the DMA address is in the guest's IPA space (most common case).
 */
err_t dma_guard_check_ipa(struct vm *vm, ipa_t ipa, u64 size);

/*
 * dma_guard_log_violation — log a DMA violation event.
 * Called from SMMU fault handler when a stream accesses an unmapped PA.
 */
void dma_guard_log_violation(u32 stream_id, u32 vm_id,
                              paddr_t fault_pa, u64 fault_size);

#endif /* HYP_DMA_GUARD_H */
