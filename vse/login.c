/*
 * login.c — VSE operator login implementation (see login.h)
 */

#include "login.h"
#include "pw_verifier.h"    /* VSE_PW_VERIFIER — provisioned per deployment    */
#include "totp.h"
#include "hmac.h"            /* hmac_sha256, hmac_verify (constant-time compare) */
#include "keystore.h"       /* vse_derive_secret — single provisioning root     */
#include "../drivers/uart/uart.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/*
 * Stored password verifier = HMAC-SHA256(pepper, password), where
 *     pepper = vse_derive_secret("vse-login-pepper-v1")   (device-bound)
 *
 * This replaces the old bare SHA-256(password): the pepper is derived from the
 * VSE master key, so the same password yields a different verifier on every
 * device (defeats precomputed/rainbow-table attacks and binds the credential to
 * the unit). The plaintext password is NEVER stored on the device.
 *
 * The verifier bytes are NOT hardcoded here — they come from VSE_PW_VERIFIER
 * (vse/pw_verifier.h), which ships NO default and FAILS the build unless set.
 * Provision the operator password per deployment WITHOUT editing this file:
 *     scripts/provision_password.sh '<password>'   (regenerates pw_verifier.h)
 * or define it at build time with -DVSE_PW_VERIFIER='{...}'. Dev/test/CI builds
 * inject a known test verifier for "changeme" (see the Makefile). See
 * pw_verifier.h.
 */
#define LOGIN_PEPPER_LABEL  "vse-login-pepper-v1"

static const u8 _pw_verifier[HMAC_SIZE] = VSE_PW_VERIFIER;

static bool _check_password(const char *pw, u32 len)
{
    u8 pepper[VSE_MASTER_KEY_LEN];
    if (FAIL(vse_derive_secret(LOGIN_PEPPER_LABEL, pepper, sizeof(pepper))))
        return false;

    u8 mac[HMAC_SIZE];
    err_t e = hmac_sha256(pepper, sizeof(pepper), (const u8 *)pw, len, mac);
    memset(pepper, 0, sizeof(pepper));         /* scrub the pepper */
    if (FAIL(e)) { memset(mac, 0, sizeof(mac)); return false; }

    bool ok = hmac_verify(_pw_verifier, mac, sizeof(mac)); /* constant-time */
    memset(mac, 0, sizeof(mac));
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
        err_t otp_e  = totp_verify(code);

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
