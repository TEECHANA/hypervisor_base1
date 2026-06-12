/*
 * stage2.h — Stage-2 page table public interface
 *
 * Phase 1 additions:
 *   SZ_2M constant for 2 MB block mapping fast-path
 *   s2_ipa_is_mapped() for overlap detection in vm_add_mem()
 */

#ifndef STAGE2_H
#define STAGE2_H

#include "../../include/types.h"
#include "../../include/error.h"

/* ── Descriptor type bits ── */
#define S2_DESC_VALID   (1ULL << 0)
#define S2_DESC_TABLE   (1ULL << 1)   /* L1/L2 table pointer when bit0 set */
#define S2_DESC_PAGE    (1ULL << 1)   /* L3 page descriptor when bit0 set  */

/* ── Stage-2 attribute bits (ARM DDI 0487, D8.3) ── */
#define S2_AF           (1ULL << 10)  /* Access flag — must be set or EL1 faults  */
#define S2_SH_INNER     (3ULL << 8)   /* Inner shareable                           */
#define S2_S2AP_RW      (3ULL << 6)   /* Read+Write, EL0+EL1                       */
#define S2_XN           (2ULL << 53)  /* Execute-never (both EL0 and EL1)          */

/* Memory attribute index encoding in Stage-2 descriptors */
#define S2_MEMATTR_WB   (0xFULL << 2) /* Write-Back Normal memory (inner+outer WB) */
#define S2_MEMATTR_DEV  (0x1ULL << 2) /* Device-nGnRE                              */

/* ── Granule sizes ── */
#define SZ_4K   0x1000ULL
#define SZ_2M   0x200000ULL
#define SZ_1G   0x40000000ULL

/* Forward declaration — vm_t is defined in core/vm/vm.h */
struct vm;

err_t  s2_create        (struct vm *vm);
err_t  s2_map           (struct vm *vm, ipa_t ipa, paddr_t pa, u64 sz, u64 flags);
err_t  s2_unmap         (struct vm *vm, ipa_t ipa, u64 sz);
bool   s2_ipa_is_mapped (struct vm *vm, ipa_t ipa, u64 sz);
void   s2_flush_tlb     (struct vm *vm);
void   s2_destroy       (struct vm *vm);

#endif /* STAGE2_H */
