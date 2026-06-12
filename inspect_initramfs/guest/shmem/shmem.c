/*
 * shmem.c — Inter-VM shared memory
 * Allocates from SHMEM_POOL_PA and maps the same physical pages into
 * both VMs' Stage-2 page tables at the requested IPA.
 */
#include "shmem.h"
#include "../../include/config.h"
#include "../../vre/mmu/stage2.h"
#include "../../core/vm/vm.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"
#include "../../arch/arm64/include/arm_regs.h"

typedef struct { paddr_t pa; u64 sz; u32 src; u32 dst; ipa_t ipa; bool active; } shm_t;
static shm_t   _regions[MAX_SHMEM_REGIONS];
static u32     _nr       = 0;
static paddr_t _cursor   = SHMEM_POOL_PA;

err_t shmem_init(void){
    memset(_regions,0,sizeof(_regions)); _nr=0; _cursor=SHMEM_POOL_PA; return E_OK;}

err_t shmem_map(u32 src_id, u32 dst_id, ipa_t ipa, u64 sz)
{
    if(_nr>=MAX_SHMEM_REGIONS) return E_NOMEM;
    if(!IS_ALIGNED(sz,4096)) return E_INVAL;
    if(_cursor+sz > SHMEM_POOL_PA+SHMEM_POOL_SZ) return E_NOMEM;

    vm_t *src=vm_by_id(src_id), *dst=vm_by_id(dst_id);
    if(!src||!dst) return E_NOTFOUND;

    paddr_t pa = _cursor; _cursor += sz;
    u64 flags =
        S2_AF |
        S2_SH_INNER |
        S2_S2AP_RW |
        S2_MEMATTR_WB;
    err_t e = s2_map(src, ipa, pa, sz, flags);
    if(FAIL(e)) return e;
    e = s2_map(dst, ipa, pa, sz, flags);
    if(FAIL(e)){ s2_unmap(src,ipa,sz); return e; }

    _regions[_nr++]=(shm_t){pa,sz,src_id,dst_id,ipa,true};
    LOG_INFO("SHMEM: VM%d<->VM%d IPA=0x%lx PA=0x%lx sz=0x%lx",
             src_id,dst_id,ipa,pa,sz);
    return E_OK;
}

err_t shmem_unmap(ipa_t ipa, u64 sz)
{
    for(u32 i=0;i<_nr;i++){
        if(!_regions[i].active||_regions[i].ipa!=ipa) continue;
        vm_t *src=vm_by_id(_regions[i].src);
        vm_t *dst=vm_by_id(_regions[i].dst);
        if(src) s2_unmap(src,ipa,sz);
        if(dst) s2_unmap(dst,ipa,sz);
        _regions[i].active=false;
        return E_OK;
    }
    return E_NOTFOUND;
}
