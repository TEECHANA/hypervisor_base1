/*
 * totp.c — VSE TOTP implementation (see totp.h for design rationale)
 *
 * Wraps hotp_compute() (the underlying RFC 4226 HMAC-OTP algorithm) with a
 * time-derived counter T = unix_now / TOTP_STEP_S, turning HOTP into TOTP
 * per RFC 6238. No counter state is persisted — time is the counter.
 */

#include "totp.h"
#include "hotp.h"
#include "hmac.h"
#include "../lib/rtc/rtc.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

static bool _initialized = false;

err_t totp_init(void)
{
    rtc_init();
    _initialized = true;
    u64 T = rtc_unix_now() / TOTP_STEP_S;
    LOG_INFO("TOTP: initialized (RFC 6238, step=%us, current T=%lu)", TOTP_STEP_S, T);
    return E_OK;
}

err_t totp_verify(const char *candidate)
{
    if (!_initialized) return E_DENIED;
    if (!candidate)    return E_INVAL;

    u64 now = rtc_unix_now();
    u64 T   = now / TOTP_STEP_S;
    char expect[HOTP_DIGITS + 1];

    /* Check T-1, T, T+1 for ±30 s clock skew tolerance (RFC 6238 §5.2). */
    u64 steps[3];
    u32 nsteps = 0;
    if (T > 0) steps[nsteps++] = T - 1;
    steps[nsteps++] = T;
    steps[nsteps++] = T + 1;

    for (u32 i = 0; i < nsteps; ++i) {
        if (FAIL(hotp_compute(steps[i], expect, sizeof(expect)))) continue;
        if (hmac_verify((const u8 *)expect, (const u8 *)candidate, HOTP_DIGITS)) {
            memset(expect, 0, sizeof(expect));
            LOG_INFO("TOTP: code verified (T=%lu, window=%s)",
                     steps[i],
                     steps[i] < T ? "T-1" : (steps[i] > T ? "T+1" : "T"));
            return E_OK;
        }
    }
    memset(expect, 0, sizeof(expect));
    return E_DENIED;
}

err_t totp_selftest(void)
{
    if (!_initialized) return E_DENIED;

    u64  T    = rtc_unix_now() / TOTP_STEP_S;
    char code[HOTP_DIGITS + 1];
    err_t e;

    e = hotp_compute(T, code, sizeof(code));
    if (FAIL(e)) {
        LOG_ERROR("TOTP selftest: compute failed (err=%d)", (int)e);
        return e;
    }

    if (OK(totp_verify(code))) {
        LOG_INFO("TOTP selftest: PASS (T=%lu, code=%s)", T, code);
        e = E_OK;
    } else {
        LOG_ERROR("TOTP selftest: FAIL — verify rejected own code at T=%lu", T);
        e = E_GENERIC;
    }

    memset(code, 0, sizeof(code));
    return e;
}
