#include "pal.h"
#include "../../include/config.h"
#include "../../include/error.h"
#include "../../vse/keystore.h"   /* VSE_MASTER_KEY_LEN */
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

/* ─────────────────────────────────────────────────────────────────────────
 * plat_read_fuse_key — device-unique key from the S32G OCOTP fuse array.
 *
 * The S32G OCOTP controller shadows each fuse word into an MMIO register at
 * boot, so a provisioned key can be read as 8 consecutive 32-bit words = 32
 * bytes (VSE_MASTER_KEY_LEN) from a customer-programmable OTP bank.
 *
 * !!! UNVALIDATED ON SILICON !!!  OCOTP_BASE and the customer-bank word offset
 * below are placeholders in the S32G memory-map range and MUST be confirmed
 * against the S32G Reference Manual (OCOTP chapter) and the fuse map your
 * manufacturing flow programs, before this is trusted. The defensive all-0 /
 * all-1 check rejects an unprogrammed bank so a blank part cannot yield a weak,
 * predictable key.
 * ───────────────────────────────────────────────────────────────────────── */
#define S32G_OCOTP_BASE        0x400A4000ULL   /* CONFIRM against S32G RM */
#define S32G_OCOTP_CUST_WORD0  0x0200u         /* customer-key bank word0 offset */

static inline u32 ocotp_r32(u64 off){
    return *(volatile u32 *)(uintptr_t)(S32G_OCOTP_BASE + off);
}

err_t plat_read_fuse_key(u8 *out, u32 len){
    if (!out || len < VSE_MASTER_KEY_LEN) return E_INVAL;

    u32 all_and = 0xFFFFFFFFu, all_or = 0u;
    for (u32 i = 0; i < 8u; ++i) {
        u32 w = ocotp_r32(S32G_OCOTP_CUST_WORD0 + i * 4u);
        all_and &= w; all_or |= w;
        out[i*4+0] = (u8)(w);       out[i*4+1] = (u8)(w >> 8);
        out[i*4+2] = (u8)(w >> 16); out[i*4+3] = (u8)(w >> 24);
    }
    if (all_or == 0u || all_and == 0xFFFFFFFFu) return E_UNSUPPORTED;
    return E_OK;
}
