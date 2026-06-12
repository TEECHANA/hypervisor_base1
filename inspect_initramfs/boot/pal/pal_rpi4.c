#include "pal.h"
#include "../../include/config.h"
#include "../../drivers/uart/uart.h"
#include "../../arch/arm64/include/arm_regs.h"
plat_t g_plat = {
    .id=PLAT_RPI4, .name="Raspberry-Pi-4",
    .uart_base=UART_BASE_RPI4, .gicd_base=0xFF841000ULL, .gicr_base=0xFF842000ULL,
    .ram_base=0x00000000ULL, .ram_size=0x100000000ULL,
    .timer_freq=54000000ULL
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
