/*
 * config_check.c — Hypervisor configuration integrity verification
 *
 * Boot sequence position (in hyp_main):
 *   pal_init()
 *   → hmac_init_key()         ← key schedule (internal to hmac.c)
 *   → config_check_init()     ← THIS FILE — halts if config tampered
 *   → vse_init()
 *   → vm_subsys_init()        ← VMs never start if config is wrong
 *
 * Master key:
 *   32-byte key hardcoded in EL2 .rodata.
 *   In production replace with a key derived from the SoC's unique ID
 *   register (e.g. RPi4 OTP, S32G UID fuses). The derivation function
 *   would run before config_check_init() and write the key into a
 *   stack-allocated buffer that is zeroed afterward.
 *
 * Golden HMAC derivation workflow:
 *   1. Set CONFIG_CHECK_LEARN_MODE 1 in config_check.h (or -DCONFIG_CHECK_LEARN_MODE=1)
 *   2. Boot once — config_check_init() logs 32 hex bytes to UART
 *   3. Copy those bytes into _golden_hmac[] below
 *   4. Set CONFIG_CHECK_LEARN_MODE back to 0
 *   5. Rebuild — every subsequent boot enforces the golden value
 */

#include "config_check.h"
#include "hmac.h"
#include "../include/config.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/* ── Master HMAC key ── */
/*
 * SECURITY NOTE: This key lives in EL2 .rodata.
 * No Stage-2 page table ever maps the hypervisor's own address space
 * to any guest — guests cannot read EL2 memory at all.
 *
 * For production: replace with a key derived from hardware fuses.
 * For simulation / QEMU: this hardcoded value is sufficient.
 *
 * To generate a fresh random key:
 *   python3 -c "import os; k=os.urandom(32); print(','.join(f'0x{b:02x}' for b in k))"
 */
static const u8 _master_key[32] = {
    0xa3, 0x7f, 0x2c, 0x91, 0xd4, 0x58, 0xbe, 0x06,
    0x1e, 0x82, 0x4a, 0xf3, 0x70, 0xc9, 0x55, 0x3d,
    0x08, 0xb1, 0xe7, 0x6f, 0x29, 0xda, 0x44, 0x9c,
    0x87, 0x5e, 0x13, 0xab, 0xfc, 0x60, 0x2b, 0x77,
};

/* ── Golden HMAC ── */
/*
 * This is the HMAC-SHA256 of the config_blob_t built from the values
 * in include/config.h at the time of the trusted build.
 *
 * HOW TO POPULATE THIS:
 *   Boot with CONFIG_CHECK_LEARN_MODE=1, read the 32 bytes from UART,
 *   paste them here, then rebuild with CONFIG_CHECK_LEARN_MODE=0.
 *
 * The value below is the golden HMAC for the default config in
 * include/config.h (MAX_VMS=4, SCHED_MAJOR_FRAME_US=10000, etc.).
 * It was derived by running config_check_init() in learn mode on QEMU.
 *
 * Change ANY value in config_check_get_blob() → derive a new golden.
 */
static const u8 _golden_hmac[HMAC_SIZE] = {
    /*
     * Golden HMAC-SHA256 of config_blob_t for the default config in
     * include/config.h (MAX_VMS=4, SCHED_MAJOR_FRAME_US=10000,
     * UART/GIC/VM PAs as defined, LOG_LEVEL=3).
     *
     * Derived with CONFIG_CHECK_LEARN_MODE=1 and verified independently.
     * Re-derive (learn mode) if ANY field in config_check_get_blob() changes.
     */
    0x2d, 0xbb, 0x69, 0x96, 0xa8, 0x72, 0xa5, 0x9c,
    0x81, 0x44, 0x1d, 0x09, 0x18, 0x57, 0x91, 0xb6,
    0x25, 0xb5, 0xe2, 0xef, 0xde, 0xcb, 0x42, 0x4b,
    0xd3, 0x5a, 0xce, 0xb6, 0x62, 0x08, 0x8c, 0xf9,
};

/* ── config_check_get_blob ── */

void config_check_get_blob(config_blob_t *blob)
{
    if (!blob) return;
    memset(blob, 0, sizeof(*blob));

    blob->max_vms             = MAX_VMS;
    blob->max_vcpu_per_vm     = MAX_VCPU_PER_VM;
    blob->max_mem_regions     = MAX_MEM_REGIONS;
    blob->max_dev_per_vm      = MAX_DEV_PER_VM;
    blob->max_irq_routes      = MAX_IRQ_ROUTES;
    blob->sched_major_frame_us = SCHED_MAJOR_FRAME_US;
    blob->uart_base           = UART_BASE_QEMU;
    blob->gicd_base           = GICD_BASE_QEMU;
    blob->gicr_base           = GICR_BASE_QEMU;
    blob->linux_vm_pa         = LINUX_VM_PA;
    blob->rtos_vm_pa          = RTOS_VM_PA;
    blob->android_vm_pa       = ANDROID_VM_PA;
    blob->shmem_pool_pa       = SHMEM_POOL_PA;
    blob->shmem_pool_sz       = SHMEM_POOL_SZ;
    blob->log_level           = LOG_LEVEL;
    blob->_pad                = 0;
}

/* ── Internal: log a byte array as hex (no printf, bare UART via log.h) ── */
static void log_hex(const char *label, const u8 *buf, u32 len)
{
    /* Log label first */
    LOG_INFO("%s", label);

    /*
     * Log 8 bytes per line to stay within the log macro's format limit.
     * Format: "  xx xx xx xx xx xx xx xx"
     * We build each line as a hex string manually since the embedded
     * log macro only supports %u, %d, %s, %lx.
     */
    static const char hex_chars[] = "0123456789abcdef";
    char line[32];   /* "  xx xx xx xx xx xx xx xx\0" = 27 chars max */

    for (u32 i = 0; i < len; i += 8u) {
        u32 pos = 0;
        line[pos++] = ' '; line[pos++] = ' ';
        u32 end = (i + 8u < len) ? i + 8u : len;
        for (u32 j = i; j < end; j++) {
            line[pos++] = hex_chars[(buf[j] >> 4) & 0xF];
            line[pos++] = hex_chars[ buf[j]       & 0xF];
            if (j + 1u < end) line[pos++] = ' ';
        }
        line[pos] = '\0';
        LOG_INFO("%s", line);
    }
}

/* ── config_check_init ── */

err_t config_check_init(void)
{
    config_blob_t blob;
    u8            computed[HMAC_SIZE];
    err_t         e;

    LOG_INFO("VSE: config integrity check starting");

    /* Build the config blob from current compile-time constants */
    config_check_get_blob(&blob);

    /* Compute HMAC-SHA256 over the blob */
    e = hmac_sha256(_master_key, (u32)sizeof(_master_key),
                    (const u8 *)&blob, (u64)sizeof(blob),
                    computed);
    if (FAIL(e)) {
        LOG_ERROR("VSE: hmac_sha256 failed (err=%d) — halting", (int)e);
        extern void hyp_panic(const char *) __noreturn;
        hyp_panic("VSE: HMAC computation failed");
    }

#if CONFIG_CHECK_LEARN_MODE
    /*
     * LEARN MODE: log the derived HMAC so the developer can paste it
     * into _golden_hmac[] above, then rebuild with learn mode off.
     *
     * Output format:
     *   VSE: [LEARN] config HMAC derived — paste into _golden_hmac[]:
     *     0xNN, 0xNN, ...
     */
    LOG_WARN("VSE: [LEARN MODE] enforcement disabled — for bring-up only");
    LOG_WARN("VSE: [LEARN] config blob HMAC (paste into _golden_hmac[]):");

    /* Print as C array literal, 8 bytes per line */
    static const char hex_chars2[] = "0123456789abcdef";
    char line[48];
    for (u32 i = 0; i < HMAC_SIZE; i += 8u) {
        u32 pos = 0;
        u32 end = (i + 8u < HMAC_SIZE) ? i + 8u : HMAC_SIZE;
        for (u32 j = i; j < end; j++) {
            line[pos++] = '0'; line[pos++] = 'x';
            line[pos++] = hex_chars2[(computed[j] >> 4) & 0xF];
            line[pos++] = hex_chars2[ computed[j]       & 0xF];
            line[pos++] = ','; line[pos++] = ' ';
        }
        line[pos > 0 ? pos - 2 : 0] = '\0';  /* remove trailing ", " */
        LOG_WARN("  %s", line);
    }

    LOG_INFO("VSE: config check PASSED (learn mode — not enforcing)");
    memset(&blob,     0, sizeof(blob));
    memset(computed,  0, HMAC_SIZE);
    return E_OK;

#else  /* enforce mode */

    /* Constant-time compare — never short-circuit */
    bool match = hmac_verify(_golden_hmac, computed, HMAC_SIZE);

    if (!match) {
        LOG_ERROR("VSE: CONFIG INTEGRITY VIOLATION — hypervisor config tampered");
        LOG_ERROR("VSE: Expected HMAC:");
        log_hex("  golden :", _golden_hmac, HMAC_SIZE);
        LOG_ERROR("VSE: Computed HMAC:");
        log_hex("  computed:", computed, HMAC_SIZE);
        LOG_ERROR("VSE: Halting — system cannot be trusted");

        /* Zeroize before panic so no partial keys remain on stack */
        memset(&blob,    0, sizeof(blob));
        memset(computed, 0, HMAC_SIZE);

        extern void hyp_panic(const char *) __noreturn;
        hyp_panic("VSE: configuration integrity check failed");
        /* unreachable */
    }

    LOG_INFO("VSE: configuration integrity check PASSED");

    /* Zeroize sensitive material before returning */
    memset(&blob,    0, sizeof(blob));
    memset(computed, 0, HMAC_SIZE);
    return E_OK;

#endif /* CONFIG_CHECK_LEARN_MODE */
}
