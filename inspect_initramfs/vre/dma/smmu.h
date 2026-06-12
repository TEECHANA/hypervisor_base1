#ifndef HYP_SMMU_H
#define HYP_SMMU_H
#include "../../include/types.h"
#include "../../include/error.h"
err_t smmu_init          (u64 base);
err_t smmu_assign_stream (u32 sid, u32 vm_id, paddr_t s2_pgd, u64 vttbr);
err_t smmu_remove_stream (u32 sid);
#endif
