#include "pal.h"
#include "../../include/config.h"
#include "../../drivers/uart/uart.h"
#include "../../arch/arm64/include/arm_regs.h"
plat_t g_plat = {
    .id=PLAT_S32G, .name="NXP-S32G",
    .uart_base=UART_BASE_S32G, .gicd_base=0x50800000ULL, .gicr_base=0x50900000ULL,
    .ram_base=0x80000000ULL, .ram_size=0x80000000ULL,
    .timer_freq=5000000ULL
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
