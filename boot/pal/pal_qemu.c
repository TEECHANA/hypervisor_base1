#include "pal.h"
#include "../../include/config.h"
#include "../../drivers/uart/uart.h"
#include "../../arch/arm64/include/arm_regs.h"
plat_t g_plat = {
    .id=PLAT_QEMU, .name="QEMU-ARM64-virt",
    .uart_base=UART_BASE_QEMU, .gicd_base=GICD_BASE_QEMU, .gicr_base=GICR_BASE_QEMU,
    .ram_base=0x40000000ULL, .ram_size=0x80000000ULL,
    .timer_freq=62500000ULL,
    .smmu_base=0x09050000ULL  /* SMMUv3 on QEMU virt with iommu=smmuv3 */
};
err_t pal_init(paddr_t dtb_pa){
    UNUSED(dtb_pa);
    g_plat.timer_freq = READ_SYSREG(cntfrq_el0);
    return uart_init(g_plat.uart_base);
}
void pal_delay_us(u32 us){
    u64 f=READ_SYSREG(cntfrq_el0),s=READ_SYSREG(cntpct_el0);
    u64 t=(f/1000000ULL)*(u64)us;
    while((READ_SYSREG(cntpct_el0)-s)<t){}
}

/*
 * QEMU virt has no on-die OTP/eFuse array, so there is no per-device key to
 * read here. This satisfies the VSE_HW_FUSE_KEY link seam but reports that the
 * hardware path is unavailable; a QEMU build should therefore use the dev-key
 * fallback (build WITHOUT VSE_HW_FUSE_KEY), not this. Failing loudly is the
 * honest behaviour — we must never silently hand back a bogus "device" key.
 */
err_t plat_read_fuse_key(u8 *out, u32 len){
    UNUSED(out); UNUSED(len);
    return E_UNSUPPORTED;
}
