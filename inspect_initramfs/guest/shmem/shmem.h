#ifndef HYP_SHMEM_H
#define HYP_SHMEM_H
#include "../../include/types.h"
#include "../../include/error.h"
err_t shmem_init  (void);
err_t shmem_map   (u32 src_vm, u32 dst_vm, ipa_t ipa, u64 sz);
err_t shmem_unmap (ipa_t ipa, u64 sz);
#endif
