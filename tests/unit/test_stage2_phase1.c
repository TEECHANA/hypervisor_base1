/*
 * test_stage2_phase1.c — self-contained Stage-2 unit tests
 *
 * Does NOT include any hypervisor header or stage2.c.
 * The page-table logic (pt_alloc, s2_map, s2_unmap, s2_ipa_is_mapped)
 * is copied verbatim from stage2.c with ARM asm replaced by no-ops.
 *
 * Build (host gcc, no cross-compiler):
 *   gcc -O0 -g -Wall -I. \
 *       tests/unit/test_stage2_phase1.c \
 *       -o build/test_stage2_p1
 *   ./build/test_stage2_p1
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* ── types (match hypervisor exactly, but using host-native u64) ── */
typedef uint64_t  u64;
typedef uint32_t  u32;
typedef uint8_t   u8;
typedef int32_t   s32;
typedef u64       paddr_t;
typedef u64       ipa_t;
typedef s32       err_t;

#define E_OK    ((err_t) 0)
#define E_INVAL ((err_t)-1)
#define E_NOMEM ((err_t)-2)

#define IS_ALIGNED(v, a)  (((u64)(v) & ((u64)(a)-1)) == 0)
#define FAIL(e)           ((e) != E_OK)

/* logging: silent during normal run, print on failure */
#define LOG_INFO(...)   do {} while(0)
#define LOG_WARN(...)   do {} while(0)
#define LOG_ERROR(f,...) fprintf(stderr, "[ERR] " f "\n", ##__VA_ARGS__)
#define LOG_DEBUG(...)  do {} while(0)

/* ── descriptor bits (from stage2.h) ── */
#define S2_DESC_VALID  (1ULL << 0)
#define S2_DESC_TABLE  (1ULL << 1)
#define S2_DESC_PAGE   (1ULL << 1)
#define S2_AF          (1ULL << 10)
#define S2_SH_INNER    (3ULL << 8)
#define S2_S2AP_RW     (3ULL << 6)
#define S2_MEMATTR_WB  (0xFULL << 2)
#define SZ_2M          0x200000ULL
#define SZ_4K          0x1000ULL

/* ── minimal vm_t ── */
typedef struct vm {
    u32     id;
    paddr_t s2_pgd;
    u64     vttbr;
} vm_t;

/* ════════════════════════════════════════════════════════════════════
 * Page-table implementation — copied verbatim from stage2.c,
 * with ARM asm stubs replaced by no-ops.
 * ════════════════════════════════════════════════════════════════════ */

#define PT_POOL_PAGES 256
static u64 _pt_pool[PT_POOL_PAGES][512] __attribute__((aligned(4096)));
static u32 _pt_idx = 0;
static u8  _vmid   = 1;

static u64 *pt_alloc(void)
{
    if (_pt_idx >= PT_POOL_PAGES) {
        fprintf(stderr, "[ERR] pt pool exhausted\n");
        exit(1);
    }
    u64 *p = _pt_pool[_pt_idx++];
    memset(p, 0, 4096);
    return p;
}

/* TLB flush — no-op on host */
static void s2_dsb_tlb_isb(void) {}

static err_t s2_create(vm_t *vm)
{
    u64 *pgd  = pt_alloc();
    vm->s2_pgd = (paddr_t)(uintptr_t)pgd;
    vm->vttbr  = ((u64)_vmid++ << 48) | (vm->s2_pgd & ~0xFFFULL);
    return E_OK;
}

static err_t s2_map(vm_t *vm, ipa_t ipa, paddr_t pa, u64 sz, u64 flags)
{
    if (!vm
        || !IS_ALIGNED(ipa, 4096)
        || !IS_ALIGNED(pa,  4096)
        || !IS_ALIGNED(sz,  4096))
        return E_INVAL;

    u64 *l1  = (u64 *)(uintptr_t)vm->s2_pgd;
    u64  off = 0;

    while (off < sz) {
        ipa_t   c_ipa = ipa + off;
        paddr_t c_pa  = pa  + off;

        /* 2 MB block fast-path */
        if (IS_ALIGNED(c_ipa, SZ_2M)
            && IS_ALIGNED(c_pa, SZ_2M)
            && (sz - off) >= SZ_2M)
        {
            u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
            u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);

            if (!(l1[i1] & 1)) {
                u64 *l2 = pt_alloc();
                l1[i1] = ((u64)(uintptr_t)l2 & ~0xFFFULL)
                        | S2_DESC_VALID | S2_DESC_TABLE;
            }
            u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

            u64 blk = (c_pa & ~(SZ_2M - 1)) | flags | S2_DESC_VALID;
            blk &= ~(1ULL << 1);   /* clear table bit → block */
            l2[i2] = blk;

            off += SZ_2M;
            continue;
        }

        /* 4 KB page path */
        u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
        u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);
        u32 i3 = (u32)((c_ipa >> 12) & 0x1FF);

        if (!(l1[i1] & 1)) {
            u64 *l2 = pt_alloc();
            l1[i1] = ((u64)(uintptr_t)l2 & ~0xFFFULL)
                   | S2_DESC_VALID | S2_DESC_TABLE;
        }
        u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

        if (!(l2[i2] & 1)) {
            u64 *l3 = pt_alloc();
            l2[i2] = ((u64)(uintptr_t)l3 & ~0xFFFULL)
                   | S2_DESC_VALID | S2_DESC_TABLE;
        }
        u64 *l3 = (u64 *)(uintptr_t)(l2[i2] & ~0xFFFULL);

        l3[i3] = (c_pa & ~0xFFFULL) | flags | S2_DESC_VALID | S2_DESC_PAGE;

        off += 4096;
    }

    s2_dsb_tlb_isb();
    return E_OK;
}

static err_t s2_unmap(vm_t *vm, ipa_t ipa, u64 sz)
{
    if (!vm
        || !IS_ALIGNED(ipa, 4096)
        || !IS_ALIGNED(sz,  4096))
        return E_INVAL;

    u64 *l1  = (u64 *)(uintptr_t)vm->s2_pgd;
    u64  off = 0;

    while (off < sz) {
        ipa_t c_ipa = ipa + off;
        u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
        u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);
        u32 i3 = (u32)((c_ipa >> 12) & 0x1FF);

        if (!(l1[i1] & 1)) { off += 4096; continue; }
        u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

        /* 2 MB block? */
        if ((l2[i2] & 1) && !(l2[i2] & 2)) {
            l2[i2] = 0;
            u64 blk_end = (c_ipa & ~(SZ_2M - 1)) + SZ_2M;
            off += (blk_end - c_ipa);
            continue;
        }

        if (!(l2[i2] & 1)) { off += 4096; continue; }
        u64 *l3 = (u64 *)(uintptr_t)(l2[i2] & ~0xFFFULL);

        if (l3[i3] & 1) l3[i3] = 0;

        off += 4096;
    }

    s2_dsb_tlb_isb();
    return E_OK;
}

static bool s2_ipa_is_mapped(vm_t *vm, ipa_t ipa, u64 sz)
{
    if (!vm) return false;
    u64 *l1 = (u64 *)(uintptr_t)vm->s2_pgd;
    u64  off;

    for (off = 0; off < sz; off += 4096) {
        ipa_t c_ipa = ipa + off;
        u32 i1 = (u32)((c_ipa >> 30) & 0x1FF);
        if (!(l1[i1] & 1)) continue;
        u64 *l2 = (u64 *)(uintptr_t)(l1[i1] & ~0xFFFULL);

        u32 i2 = (u32)((c_ipa >> 21) & 0x1FF);
        if (!(l2[i2] & 1)) continue;
        if (!(l2[i2] & 2)) return true;   /* 2 MB block */

        u64 *l3 = (u64 *)(uintptr_t)(l2[i2] & ~0xFFFULL);
        u32 i3  = (u32)((c_ipa >> 12) & 0x1FF);
        if (l3[i3] & 1) return true;
    }
    return false;
}

/* ════════════════════════════════════════════════════════════════════
 * Test harness
 * ════════════════════════════════════════════════════════════════════ */

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  \xE2\x9C\x93  %s\n", msg); \
        g_pass++; \
    } else { \
        printf("  \xE2\x9C\x97  FAIL: %s  (line %d)\n", msg, __LINE__); \
        g_fail++; \
    } \
} while(0)

static void reset_pool(void) { _pt_idx = 0; _vmid = 1; }

/* ── test 1: basic map + is_mapped ── */
static void test_map_and_is_mapped(void)
{
    vm_t vm;
    u64 flags = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;

    printf("\n--- test_map_and_is_mapped ---\n");
    reset_pool();
    memset(&vm, 0, sizeof(vm));
    s2_create(&vm);

    ASSERT(s2_map(&vm, 0x1000, 0xA001000, 0x1000, flags) == E_OK,
           "s2_map single 4 KB page returns E_OK");
    ASSERT(s2_ipa_is_mapped(&vm, 0x1000, 0x1000),
           "is_mapped true after map");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x2000, 0x1000),
           "is_mapped false for never-mapped page");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x0000, 0x1000),
           "is_mapped false for adjacent lower page");
}

/* ── test 2: unmap clears PTEs ── */
static void test_unmap_clears_pte(void)
{
    vm_t vm;
    u64 flags = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;

    printf("\n--- test_unmap_clears_pte ---\n");
    reset_pool();
    memset(&vm, 0, sizeof(vm));
    s2_create(&vm);

    /* Map 3 contiguous pages */
    ASSERT(s2_map(&vm, 0x4000, 0xB004000, 0x3000, flags) == E_OK,
           "s2_map 3 pages returns E_OK");
    ASSERT(s2_ipa_is_mapped(&vm, 0x4000, 0x3000), "3 pages mapped");

    /* Unmap middle page only */
    ASSERT(s2_unmap(&vm, 0x5000, 0x1000) == E_OK,
           "s2_unmap middle page returns E_OK");
    ASSERT(s2_ipa_is_mapped(&vm, 0x4000, 0x1000),  "first page still mapped");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x5000, 0x1000), "middle page unmapped");
    ASSERT(s2_ipa_is_mapped(&vm, 0x6000, 0x1000),  "last page still mapped");

    /* Unmap rest */
    s2_unmap(&vm, 0x4000, 0x1000);
    s2_unmap(&vm, 0x6000, 0x1000);
    ASSERT(!s2_ipa_is_mapped(&vm, 0x4000, 0x3000), "all 3 pages unmapped");
}

/* ── test 3: 2 MB block fast-path ── */
static void test_2mb_block(void)
{
    vm_t vm;
    u64 flags = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;

    printf("\n--- test_2mb_block_fast_path ---\n");
    reset_pool();
    memset(&vm, 0, sizeof(vm));
    s2_create(&vm);

    /* PGD already consumed one page; note pool position */
    u32 pool_before = _pt_idx;

    /* 2 MB aligned IPA + PA */
    ASSERT(s2_map(&vm, 0x200000, 0x40200000, SZ_2M, flags) == E_OK,
           "s2_map 2 MB block returns E_OK");

    u32 pool_after = _pt_idx;
    /* Block map must allocate at most 1 L2 table (no L3) */
    ASSERT((pool_after - pool_before) <= 1,
           "2 MB block uses at most 1 new PT page (no L3 allocated)");

    ASSERT(s2_ipa_is_mapped(&vm, 0x200000, SZ_2M),
           "full 2 MB range reports mapped");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x000000, 0x1000),
           "page before block not mapped");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x400000, 0x1000),
           "page after block not mapped");

    /* Unmap the block */
    ASSERT(s2_unmap(&vm, 0x200000, SZ_2M) == E_OK,
           "s2_unmap 2 MB block returns E_OK");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x200000, SZ_2M),
           "2 MB range unmapped after s2_unmap");
}

/* ── test 4: overlap detection ── */
static void test_overlap_detection(void)
{
    vm_t vm;
    u64 flags = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;

    printf("\n--- test_overlap_detection ---\n");
    reset_pool();
    memset(&vm, 0, sizeof(vm));
    s2_create(&vm);

    s2_map(&vm, 0x10000, 0xC010000, 0x4000, flags);   /* 4 pages */

    /* Pages inside the mapped range */
    ASSERT(s2_ipa_is_mapped(&vm, 0x10000, 0x1000), "start of range mapped");
    ASSERT(s2_ipa_is_mapped(&vm, 0x12000, 0x1000), "middle of range mapped");
    ASSERT(s2_ipa_is_mapped(&vm, 0x13000, 0x1000), "end of range mapped");

    /* Adjacent pages outside */
    ASSERT(!s2_ipa_is_mapped(&vm, 0x0F000, 0x1000), "page before range not mapped");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x14000, 0x1000), "page after range not mapped");

    /* Straddling range: starts outside, overlaps inside */
    ASSERT(s2_ipa_is_mapped(&vm, 0x13000, 0x2000),
           "straddling range detects the mapped page");
}

/* ── test 5: mixed 2 MB block + 4 KB page in same map call ── */
static void test_mixed_granule(void)
{
    vm_t vm;
    u64 flags = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;

    printf("\n--- test_mixed_granule ---\n");
    reset_pool();
    memset(&vm, 0, sizeof(vm));
    s2_create(&vm);

    /*
     * Map 2 MB + 4 KB starting at a 2 MB aligned address.
     * The first 2 MB should use a block; the trailing 4 KB a page.
     */
    u64 sz = SZ_2M + SZ_4K;
    ASSERT(s2_map(&vm, 0x200000, 0x40200000, sz, flags) == E_OK,
           "mixed map (2 MB block + 4 KB tail) returns E_OK");

    ASSERT(s2_ipa_is_mapped(&vm, 0x200000, SZ_2M),
           "2 MB block portion mapped");
    ASSERT(s2_ipa_is_mapped(&vm, 0x200000 + SZ_2M, SZ_4K),
           "4 KB tail page mapped");
    ASSERT(!s2_ipa_is_mapped(&vm, 0x200000 + sz, SZ_4K),
           "page beyond tail not mapped");

    /* Unmap the whole thing */
    s2_unmap(&vm, 0x200000, sz);
    ASSERT(!s2_ipa_is_mapped(&vm, 0x200000, sz),
           "full mixed range unmapped");
}

/* ── test 6: invalid argument rejection ── */
static void test_invalid_args(void)
{
    vm_t vm;
    u64 flags = S2_AF | S2_SH_INNER | S2_S2AP_RW | S2_MEMATTR_WB;

    printf("\n--- test_invalid_args ---\n");
    reset_pool();
    memset(&vm, 0, sizeof(vm));
    s2_create(&vm);

    ASSERT(s2_map(&vm, 0x1001, 0xA000000, 0x1000, flags) == E_INVAL,
           "unaligned IPA rejected");
    ASSERT(s2_map(&vm, 0x1000, 0xA000001, 0x1000, flags) == E_INVAL,
           "unaligned PA rejected");
    ASSERT(s2_map(&vm, 0x1000, 0xA000000, 0x1001, flags) == E_INVAL,
           "unaligned size rejected");
    ASSERT(s2_map(NULL, 0x1000, 0xA000000, 0x1000, flags) == E_INVAL,
           "NULL vm rejected by s2_map");
    ASSERT(s2_unmap(NULL, 0x1000, 0x1000) == E_INVAL,
           "NULL vm rejected by s2_unmap");
}

int main(void)
{
    printf("=== Stage-2 Phase 1 unit tests ===\n");

    test_map_and_is_mapped();
    test_unmap_clears_pte();
    test_2mb_block();
    test_overlap_detection();
    test_mixed_granule();
    test_invalid_args();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
