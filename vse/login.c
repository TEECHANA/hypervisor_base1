/*
 * login.c — VSE operator login implementation (see login.h)
 */

#include "login.h"
#include "hotp.h"
#include "hmac.h"            /* sha256_*, hmac_verify (constant-time compare) */
#include "../drivers/uart/uart.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/*
 * Stored password hash = SHA-256("changeme").
 *
 * Provision a real one with scripts/hotp_gen.py --pwhash <password> and paste
 * the 32 bytes here (or have seal_wrapper.sh inject it at build time). The
 * plaintext password is NEVER stored on the device. Default shown so the
 * subsystem is testable out of the box — change before deployment.
 *
 * SHA-256("changeme") =
 *   057ba03d6c44104863dc7361fe4578965d1887360f90a0895882e58a6248fc86
 */
static const u8 _pw_hash[32] = {
    0x05,0x7b,0xa0,0x3d,0x6c,0x44,0x10,0x48,
    0x63,0xdc,0x73,0x61,0xfe,0x45,0x78,0x96,
    0x5d,0x18,0x87,0x36,0x0f,0x90,0xa0,0x89,
    0x58,0x82,0xe5,0x8a,0x62,0x48,0xfc,0x86,
};

static bool _check_password(const char *pw, u32 len)
{
    u8 digest[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const u8 *)pw, len);
    sha256_final(&ctx, digest);

    bool ok = hmac_verify(_pw_hash, digest, sizeof(digest)); /* constant-time */
    memset(digest, 0, sizeof(digest));
    return ok;
}

err_t login_authenticate(void)
{
    char pw[LOGIN_PW_MAX];
    char code[16];

    uart_puts("\n========================================\n");
    uart_puts("  Tessolve Hypervisor — Operator Login\n");
    uart_puts("========================================\n");

    for (u32 attempt = 1; attempt <= LOGIN_MAX_TRIES; ++attempt) {
        uart_puts("Password: ");
        u32 pwlen = uart_getline(pw, sizeof(pw), '*');

        uart_puts("OTP code: ");
        u32 clen = uart_getline(code, sizeof(code), 0);
        (void)clen;

        bool pw_ok   = _check_password(pw, pwlen);
        err_t otp_e  = hotp_verify(code);

        /* Scrub secrets from the stack immediately. */
        memset(pw, 0, sizeof(pw));

        if (pw_ok && OK(otp_e)) {
            memset(code, 0, sizeof(code));
            uart_puts("Access granted.\n\n");
            LOG_INFO("LOGIN: operator authenticated (attempt %d)", (int)attempt);
            return E_OK;
        }

        memset(code, 0, sizeof(code));
        uart_puts("Access denied.\n");
        /* Log which factor failed for the audit trail, but don't tell the
         * console (avoid helping an attacker narrow it down). */
        LOG_WARN("LOGIN: failed attempt %d (pw_ok=%d otp_ok=%d)",
                 (int)attempt, (int)pw_ok, (int)OK(otp_e));
    }

    uart_puts("Maximum attempts exceeded. System locked.\n");
    LOG_ERROR("LOGIN: lockout after %d failed attempts", (int)LOGIN_MAX_TRIES);
    return E_DENIED;
}
