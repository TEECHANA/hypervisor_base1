#include <stdio.h>
#include <assert.h>
#include "../../include/types.h"
#include "../../include/error.h"
#include "../../include/config.h"
#include "../../arch/arm64/include/arm_regs.h"
#define __asm__(...)  /* stub asm for host */
#include "../../lib/str/string.c"
typedef struct vm { u32 id; u64 s2_pgd; u64 vttbr; u32 num_vcpus; } vm_t;
/* Provide stub for s2_flush_tlb asm */
void s2_flush_tlb_stub(void){}
#include "../../vre/mmu/stage2.c"
int main(void){
    vm_t vm={.id=1};
    assert(s2_create(&vm)==E_OK);
    assert(vm.s2_pgd && vm.vttbr);
    u64 flags = S2_AF|S2_SH_IS|S2_AP_RW|S2_ATTR_NORM|S2_PAGE;
    assert(s2_map(&vm,0ULL,0x41000000ULL,4096,flags)==E_OK);
    printf("Stage-2 tests passed.\n"); return 0;
}
