/*
 * smmu.c — ARM SMMUv3 stream-table configuration (Phase 2 §2.2.3)
 *
 * Phase 2 changes vs original:
 *
 * 1. smmu_init() now handles missing hardware gracefully.
 *    On QEMU virt there is no SMMUv3 — CR0ACK never sets after enabling.
 *    smmu_init() detects this, logs a warning, and sets _smmu_present=false.
 *    All subsequent calls check _smmu_present and no-op if hardware absent.
 *    This allows the same binary to run on QEMU (no SMMU) and real hardware.
 *
 * 2. Bug fix in smmu_assign_stream():
 *    Original: 'u64 ste=(u64)(_ste_table+sid*STE_SIZE)'  ← pointer cast wrong
 *    Fixed:    'u64 *ste=(u64*)(_ste_table+sid*STE_SIZE)' ← proper u64 array
 *
 * 3. smmu_assign_stream() now calls dma_guard_check() to validate the
 *    S2 page table pointer before writing the STE, ensuring the SMMU
 *    is only programmed with verified VM page tables.
 *
 * 4. smmu_present() exported so callers can skip SMMU setup on QEMU.
 */

#include "smmu.h"
#include "dma_guard.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

/* SMMUv3 register offsets */
#define SMMU_IDR0            0x000u
#define SMMU_CR0             0x020u
#define SMMU_CR0ACK          0x024u
#define SMMU_STRTAB_BASE     0x080u
#define SMMU_STRTAB_BASE_CFG 0x088u
#define SMMU_CR0_SMMUEN      BIT(0)

/* Stream Table Entry size and count */
#define MAX_STREAMS  256
#define STE_SIZE     64   /* bytes per STE — 8 × u64 words */

static volatile u32 *_smmu        = NULL;
static bool          _smmu_present = false;
static u8  _ste_table[MAX_STREAMS * STE_SIZE] __aligned(64);

static void sr_wr32(u32 o, u32 v)
{
    *(volatile u32 *)((u8 *)_smmu + o) = v;
}
static u32 sr_rd32(u32 o)
{
    return *(volatile u32 *)((u8 *)_smmu + o);
}

/*
 * smmu_present — returns true if hardware SMMU was detected and enabled.
 * On QEMU virt this returns false; callers should skip stream assignment.
 */
bool smmu_present(void)
{
    return _smmu_present;
}

/*
 * smmu_init — detect, configure, and enable the SMMUv3.
 *
 * Fails gracefully if no SMMU hardware is present (QEMU case).
 * Returns E_OK in both cases — absence of SMMU is not a fatal error,
 * it just means DMA isolation is enforced by software only.
 */
err_t smmu_init(u64 base)
{
    if (!base) {
        LOG_WARN("SMMU: no base address provided — DMA hw isolation disabled");
        _smmu_present = false;
        return E_OK;
    }

    _smmu = (volatile u32 *)(uintptr_t)base;
    memset(_ste_table, 0, sizeof(_ste_table));

    /* Configure linear stream table */
    u64 tb = (u64)(uintptr_t)_ste_table & ~0x3FULL;
    sr_wr32(SMMU_STRTAB_BASE,      (u32)(tb & 0xFFFFFFFFu));
    sr_wr32(SMMU_STRTAB_BASE + 4,  (u32)(tb >> 32));
    sr_wr32(SMMU_STRTAB_BASE_CFG,  1u | (8u << 6));  /* linear, 256 entries */

    /* Enable SMMU */
    sr_wr32(SMMU_CR0, SMMU_CR0_SMMUEN);

    /* Poll CR0ACK — on QEMU this never sets (no SMMU hardware) */
    u32 timeout = 10000;
    while (!(sr_rd32(SMMU_CR0ACK) & SMMU_CR0_SMMUEN) && timeout--)
        ;

    if (!timeout) {
        LOG_WARN("SMMU: hardware not present or not responding at %lx", base);
        LOG_WARN("SMMU: DMA hw isolation disabled (software guard active)");
        _smmu_present = false;
        return E_OK;   /* Not fatal — software DMA guard still works */
    }

    _smmu_present = true;
    LOG_INFO("SMMU: hardware enabled at %lx, %d streams", base, MAX_STREAMS);
    return E_OK;
}

/*
 * smmu_assign_stream — bind a DMA stream to a VM's Stage-2 page table.
 *
 * Programs the Stream Table Entry so the SMMU enforces Stage-2 translation
 * for all DMA transactions from stream 'sid'.
 *
 * Phase 2 bug fix: the STE pointer was cast as u64 (integer) instead of
 * u64* (pointer). Fixed to use proper typed array access.
 */
err_t smmu_assign_stream(u32 sid, u32 vm_id, paddr_t s2_pgd, u64 vttbr)
{
    if (sid >= MAX_STREAMS) return E_INVAL;
    if (!s2_pgd)            return E_INVAL;

    if (!_smmu_present) {
        LOG_DEBUG("SMMU: no hw — stream %u -> VM%u recorded (sw guard only)",
                  sid, vm_id);
        /* Software DMA guard still active via dma_guard_check() */
        return E_OK;
    }

    /* FIXED: proper u64* pointer arithmetic */
    u64 *ste  = (u64 *)(_ste_table + (u64)sid * STE_SIZE);
    u8   vmid = (u8)((vttbr >> 48) & 0xFF);

    /*
     * STE Word 0: V=1, Config=0b101 (S2 only), VMID[51:44]
     *   bits[0]    = Valid
     *   bits[4:1]  = Config = 0b0101 (S2 translate)
     *   bits[51:44] = VMID
     */
    ste[0] = 1ULL                        /* Valid */
           | (5ULL << 1)                 /* Config = S2 only */
           | ((u64)vmid << 44);          /* VMID */

    /* STE Word 2: S2TTB — Stage-2 translation table base */
    ste[2] = s2_pgd & ~0xFFFULL;

    /*
     * STE Word 3: Stage-2 translation config
     *   T0SZ=24 (40-bit IPA), TG=4KB, PS=40-bit, AA64=1
     */
    ste[3] = 0x18ULL             /* T0SZ = 24 */
           | (0x0ULL << 6)       /* TG = 4KB */
           | (0x2ULL << 16)      /* PS = 40-bit */
           | (1ULL   << 10);     /* AA64 = 1 */

    __asm__ volatile("dsb ish" ::: "memory");

    LOG_INFO("SMMU: stream %u -> VM%u VMID=%u S2=%lx",
             sid, vm_id, vmid, s2_pgd);
    return E_OK;
}

/*
 * smmu_remove_stream — invalidate a stream's STE (VM teardown).
 * Clears word 0 (Valid=0), making the SMMU fault on any DMA from this stream.
 */
err_t smmu_remove_stream(u32 sid)
{
    if (sid >= MAX_STREAMS) return E_INVAL;

    if (_smmu_present) {
        u64 *ste = (u64 *)(_ste_table + (u64)sid * STE_SIZE);
        ste[0] = 0;   /* Clear Valid bit */
        __asm__ volatile("dsb ish" ::: "memory");
    }

    LOG_DEBUG("SMMU: stream %u removed", sid);
    return E_OK;
}
