#include "s2_debug.h"
#include "../../include/types.h"
#include "../../drivers/uart/uart.h"
#include "../../lib/log/log.h"
#define PAGE_SHIFT 12
#define INDEX_MASK 0x1FFUL

void s2_walk(u64 *root, u64 ipa)
{
    u64 l1i = (ipa >> 30) & INDEX_MASK;
    u64 l2i = (ipa >> 21) & INDEX_MASK;
    u64 l3i = (ipa >> 12) & INDEX_MASK;

    LOG_INFO("S2 WALK IPA=0x%lx", ipa);

    u64 l1e = root[l1i];
    LOG_INFO("  L1[%lu] = 0x%lx %s", l1i, l1e,
             (l1e & 3) == 3 ? "(table)" :
             (l1e & 3) == 1 ? "(block)" : "(INVALID)");

    if ((l1e & 3) != 3) {
        LOG_WARN("  S2 walk: L1 entry INVALID or block — IPA not mapped at L2");
        return;
    }

    u64 *l2 = (u64 *)(uintptr_t)(l1e & 0x0000FFFFFFFFF000ULL);
    u64 l2e = l2[l2i];
    LOG_INFO("  L2[%lu] = 0x%lx %s", l2i, l2e,
             (l2e & 3) == 3 ? "(table)" :
             (l2e & 3) == 1 ? "(block)" : "(INVALID)");

    if ((l2e & 3) != 3) {
        LOG_WARN("  S2 walk: L2 entry INVALID or block — IPA not mapped at L3");
        return;
    }

    u64 *l3 = (u64 *)(uintptr_t)(l2e & 0x0000FFFFFFFFF000ULL);
    u64 l3e = l3[l3i];
    LOG_INFO("  L3[%lu] = 0x%lx %s", l3i, l3e,
             (l3e & 3) == 3 ? "(page, valid)" : "(INVALID)");

    if ((l3e & 3) == 3) {
        u64 mapped_pa = (l3e & 0x0000FFFFFFFFF000ULL) | (ipa & 0xFFFULL);
        LOG_INFO("  IPA 0x%lx -> PA 0x%lx  ✓", ipa, mapped_pa);
    } else {
        LOG_WARN("  S2 walk: L3 entry INVALID — page not present");
    }
}
