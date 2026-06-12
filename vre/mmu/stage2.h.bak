#ifndef HYP_STAGE2_H
#define HYP_STAGE2_H
#include "../../include/types.h"
#include "../../include/error.h"
struct vm;
err_t s2_create   (struct vm *vm);
err_t s2_map      (struct vm *vm, ipa_t ipa, paddr_t pa, u64 sz, u64 flags);
err_t s2_unmap    (struct vm *vm, ipa_t ipa, u64 sz);
void  s2_flush_tlb(struct vm *vm);
void  s2_destroy  (struct vm *vm);
#endif
