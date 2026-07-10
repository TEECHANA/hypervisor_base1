#include "pal.h"
#include "../../include/config.h"
#include "../../include/error.h"
#include "../../vse/keystore.h"   /* VSE_MASTER_KEY_LEN */
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

/* ─────────────────────────────────────────────────────────────────────────
 * plat_read_fuse_key — device-unique key from the Pi4 "customer OTP" rows.
 *
 * On BCM2711 the OTP is not directly MMIO-mapped to the ARM; it is read through
 * the VideoCore mailbox property interface (channel 8, tag 0x00030021
 * GET_CUSTOMER_OTP). Customer OTP is 8 x 32-bit rows = 32 bytes, which is
 * exactly VSE_MASTER_KEY_LEN. The rows must be programmed once per unit during
 * manufacturing provisioning.
 *
 * !!! UNVALIDATED ON SILICON !!!  The mailbox sequence and the peripheral base
 * (0xFE00_B880 on Pi4) are per the BCM2711 datasheet, but this has NOT been run
 * on hardware. Before trusting it: confirm the base, the bus-address alias used
 * in the message buffer, and cache coherency of `msg[]`. Defensive checks below
 * reject an all-zero / all-ones (unprogrammed) OTP so a blank part cannot yield
 * a weak, predictable key.
 * ───────────────────────────────────────────────────────────────────────── */
#define RPI4_MBOX_BASE     0xFE00B880ULL
#define MBOX_READ          0x00u
#define MBOX_STATUS        0x18u
#define MBOX_WRITE         0x20u
#define MBOX_FULL          0x80000000u
#define MBOX_EMPTY         0x40000000u
#define MBOX_CH_PROP       8u
#define TAG_GET_CUSTOMER_OTP 0x00030021u

static inline u32 mmio_r32(u64 a){ return *(volatile u32 *)(uintptr_t)a; }
static inline void mmio_w32(u64 a, u32 v){ *(volatile u32 *)(uintptr_t)a = v; }

err_t plat_read_fuse_key(u8 *out, u32 len){
    if (!out || len < VSE_MASTER_KEY_LEN) return E_INVAL;

    /* 16-byte-aligned property-tag message: 8 OTP rows requested. */
    static volatile u32 __attribute__((aligned(16))) msg[16];
    msg[0] = sizeof(msg);            /* total size            */
    msg[1] = 0;                      /* request code          */
    msg[2] = TAG_GET_CUSTOMER_OTP;   /* tag id                */
    msg[3] = 8u * 4u;                /* response buffer bytes */
    msg[4] = 8u;                     /* request length        */
    msg[5] = 0u;                     /* row offset            */
    msg[6] = 8u;                     /* number of rows        */
    /* msg[7..14] receive the 8 OTP rows */
    msg[15] = 0u;                    /* end tag               */

    u32 addr = (u32)((uintptr_t)msg) | MBOX_CH_PROP;   /* low nibble = channel */

    while (mmio_r32(RPI4_MBOX_BASE + MBOX_STATUS) & MBOX_FULL) { }
    mmio_w32(RPI4_MBOX_BASE + MBOX_WRITE, addr);

    for (;;) {
        while (mmio_r32(RPI4_MBOX_BASE + MBOX_STATUS) & MBOX_EMPTY) { }
        if (mmio_r32(RPI4_MBOX_BASE + MBOX_READ) == addr) break;
    }

    if (msg[1] != 0x80000000u) return E_GENERIC;       /* VC did not ACK */

    u32 all_and = 0xFFFFFFFFu, all_or = 0u;
    for (u32 i = 0; i < 8u; ++i) {
        u32 w = msg[7 + i];
        all_and &= w; all_or |= w;
        out[i*4+0] = (u8)(w >> 24); out[i*4+1] = (u8)(w >> 16);
        out[i*4+2] = (u8)(w >> 8);  out[i*4+3] = (u8)(w);
    }
    /* Blank OTP reads as all-0 or all-1 -> refuse rather than issue a weak key. */
    if (all_or == 0u || all_and == 0xFFFFFFFFu) return E_UNSUPPORTED;
    return E_OK;
}
