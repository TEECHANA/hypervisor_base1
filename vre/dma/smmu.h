/*
 * smmu.h — ARM SMMUv3 driver interface (Phase 2 §2.2.3)
 *
 * Phase 2 addition: smmu_present() so callers can skip stream
 * assignment on platforms without SMMU hardware (e.g. QEMU virt).
 */

#ifndef HYP_SMMU_H
#define HYP_SMMU_H

#include "../../include/types.h"
#include "../../include/error.h"

/*
 * smmu_init — detect and enable SMMU hardware.
 * Returns E_OK even if no hardware is present (graceful degradation).
 * Check smmu_present() after calling smmu_init() to know if hw is active.
 */
err_t smmu_init         (u64 base);

/*
 * smmu_present — true if SMMU hardware was detected and enabled.
 * If false, DMA isolation is software-only (dma_guard.c).
 */
bool  smmu_present      (void);

err_t smmu_assign_stream(u32 sid, u32 vm_id, paddr_t s2_pgd, u64 vttbr);
err_t smmu_remove_stream(u32 sid);

#endif /* HYP_SMMU_H */
