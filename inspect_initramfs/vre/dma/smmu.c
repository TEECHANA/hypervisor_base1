/*
 * smmu.c — ARM SMMUv3 stream-table configuration
 * Assigns DMA streams to Stage-2 page tables so peripherals are
 * confined to their VM's physical memory.
 */
#include "smmu.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

/* SMMUv3 register offsets */
#define SMMU_IDR0          0x000u
#define SMMU_CR0           0x020u
#define SMMU_CR0ACK        0x024u
#define SMMU_STRTAB_BASE   0x080u
#define SMMU_STRTAB_BASE_CFG 0x088u
#define SMMU_CR0_SMMUEN    BIT(0)

#define MAX_STREAMS        256
#define STE_SIZE           64    /* bytes per Stream Table Entry */

static volatile u32 *_smmu;
static u8 _ste_table[MAX_STREAMS * STE_SIZE] __aligned(64);

static void sr_wr32(u32 o,u32 v){*(volatile u32*)((u8*)_smmu+o)=v;}
static u32  sr_rd32(u32 o){return *(volatile u32*)((u8*)_smmu+o);}

err_t smmu_init(u64 base)
{
    _smmu = (volatile u32*)(uintptr_t)base;
    memset(_ste_table,0,sizeof(_ste_table));

    /* Configure linear stream table */
    u64 tb = (u64)(uintptr_t)_ste_table & ~0x3FULL;
    sr_wr32(SMMU_STRTAB_BASE,     (u32)(tb & 0xFFFFFFFFu));
    sr_wr32(SMMU_STRTAB_BASE+4,   (u32)(tb >> 32));
    sr_wr32(SMMU_STRTAB_BASE_CFG, 1u|(8u<<6));  /* linear, 256 entries */

    /* Enable */
    sr_wr32(SMMU_CR0, SMMU_CR0_SMMUEN);
    u32 t=10000; while(!(sr_rd32(SMMU_CR0ACK)&SMMU_CR0_SMMUEN)&&t--){}
    LOG_INFO("SMMU init @ 0x%lx", base);
    return E_OK;
}

err_t smmu_assign_stream(u32 sid, u32 vm_id, paddr_t s2_pgd, u64 vttbr)
{
    if(sid>=MAX_STREAMS) return E_INVAL;
    UNUSED(vm_id);
    u64 *ste=(u64*)(_ste_table+sid*STE_SIZE);
    u8 vmid=(u8)((vttbr>>48)&0xFF);
    /* STE Word0: V=1, Config=0b101 (S2 only), VMID */
    ste[0]=1ULL|(5ULL<<1)|((u64)vmid<<44);
    /* STE Word2: S2TTB */
    ste[2]=s2_pgd&~0xFFFULL;
    /* STE Word3: T0SZ=24, TG=4KB, PS=40b, AA64=1 */
    ste[3]=0x18ULL|(0x0ULL<<6)|(0x2ULL<<16)|(1ULL<<10);
    __asm__ volatile("dsb ish":::"memory");
    LOG_DEBUG("SMMU: stream %d -> VM%d S2=0x%lx", sid, vm_id, s2_pgd);
    return E_OK;
}

err_t smmu_remove_stream(u32 sid){
    if(sid>=MAX_STREAMS) return E_INVAL;
    u64 *ste=(u64*)(_ste_table+sid*STE_SIZE); ste[0]=0;
    __asm__ volatile("dsb ish":::"memory");
    return E_OK;
}
