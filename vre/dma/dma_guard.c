/*
 * dma_guard.c — DMA address and protection manager (Phase 2 §2.2.2)
 *
 * Implementation notes:
 *
 * dma_guard_check() uses s2_ipa_is_mapped() from Phase 1 to validate
 * that the target physical address range is within a VM's Stage-2
 * address space. For QEMU virt VMs whose IPA base == PA base (e.g.
 * RTOS: IPA 0 → PA 0x60000000 with a flat offset), the caller must
 * convert PA to IPA before calling dma_guard_check_ipa().
 *
 * dma_guard_check() takes a raw PA and checks it against each of the
 * VM's mem_region_t entries, computing the corresponding IPA:
 *   IPA = ipa_base + (PA - pa_base)
 * This avoids requiring the caller to know the IPA offset.
 */

#include "dma_guard.h"
#include "../../core/vm/vm.h"
#include "../../vre/mmu/stage2.h"
#include "../../lib/log/log.h"
#include "../../vse/fault_detect.h"   /* VSE Phase 5 */

err_t dma_guard_check(struct vm *vm, paddr_t pa, u64 size)
{
    if (!vm || !size) return E_INVAL;
    if (size & 0xFFF) {
        /* Round up to page boundary for the check */
        size = (size + 0xFFF) & ~0xFFFULL;
    }

    /*
     * Walk the VM's mem_region table to find which region contains
     * this PA, then compute the corresponding IPA and call
     * s2_ipa_is_mapped() to verify the Stage-2 mapping exists.
     */
    for (u32 i = 0; i < vm->num_mem; i++) {
        mem_region_t *r = &vm->mem[i];
        if (pa >= r->pa_base && pa < r->pa_base + r->size) {
            /* PA is within this region — compute IPA */
            ipa_t ipa = r->ipa_base + (pa - r->pa_base);

            /* Verify the entire DMA range fits within this region */
            if (pa + size > r->pa_base + r->size) {
                LOG_WARN("DMA: VM%d range PA=%lx+%lx crosses region boundary",
                         vm->id, pa, size);
                return E_INVAL;
            }

            /* Verify Stage-2 mapping exists */
            if (!s2_ipa_is_mapped(vm, ipa, size)) {
                LOG_WARN("DMA: VM%d IPA=%lx+%lx not mapped in S2",
                         vm->id, ipa, size);
                return E_INVAL;
            }

            LOG_DEBUG("DMA: VM%d PA=%lx+%lx OK (IPA=%lx)",
                      vm->id, pa, size, ipa);
            return E_OK;
        }
    }

    /* PA not in any of this VM's regions */
    LOG_WARN("DMA: VM%d PA=%lx+%lx outside all mapped regions — DENIED",
             vm->id, pa, size);
    return E_INVAL;
}

err_t dma_guard_check_ipa(struct vm *vm, ipa_t ipa, u64 size)
{
    if (!vm || !size) return E_INVAL;
    if (size & 0xFFF)
        size = (size + 0xFFF) & ~0xFFFULL;

    if (!s2_ipa_is_mapped(vm, ipa, size)) {
        LOG_WARN("DMA: VM%d IPA=%lx+%lx not in S2 — DENIED",
                 vm->id, ipa, size);
        return E_INVAL;
    }

    LOG_DEBUG("DMA: VM%d IPA=%lx+%lx OK", vm->id, ipa, size);
    return E_OK;
}

void dma_guard_log_violation(u32 stream_id, u32 vm_id,
                              paddr_t fault_pa, u64 fault_size)
{
    LOG_ERROR("DMA VIOLATION: stream=%u VM%u PA=%lx size=%lx",
              stream_id, vm_id, fault_pa, fault_size);
    LOG_ERROR("DMA VIOLATION: peripheral attempted out-of-bounds DMA");

    /* VSE Phase 5: report to trust engine */
    fdetect_dma_violation(vm_id, fault_pa, fault_size, stream_id);
    /*
     * Phase 3: inject a virtual SError to the offending VM here.
     * For now, log and continue — the SMMU hardware will abort
     * the transaction at the bus level.
     */
}
