/*
 * totp.h — VSE: TOTP operator login OTP (RFC 6238)
 *
 * Implements TOTP on top of the in-tree HMAC-SHA256 and the lib/rtc soft RTC.
 * The time step T = floor(unix_now / TOTP_STEP_S) is used as the HOTP counter,
 * so the code changes every TOTP_STEP_S seconds. Accepts T-1 and T+1 to
 * tolerate up to ±30 s of clock skew between the hypervisor and the operator's
 * authenticator app.
 *
 * Algorithm (RFC 6238 §4 over RFC 4226):
 *   T       = floor(unix_now / step)
 *   code    = HOTP(secret, T)       — using HMAC-SHA256 (see hotp.c)
 *
 * Host tool: scripts/totp_gen.py generates matching codes given the same secret
 * and provisioned epoch (RTC_PROVISIONED_EPOCH from lib/rtc/rtc.h).
 */

#ifndef VSE_TOTP_H
#define VSE_TOTP_H

#include "../include/types.h"
#include "../include/error.h"

/* TOTP validity window per RFC 6238 §5.2. */
#define TOTP_STEP_S    30u

/*
 * totp_init — initialize TOTP: start the soft RTC so unix_now is available.
 * Must be called after seal_init() (Phase 4) if the underlying HOTP counter
 * store is used. In TOTP mode the time step replaces the counter, so no
 * persistent counter blob is required — call order only matters for rtc_init().
 */
err_t totp_init(void);

/*
 * totp_verify — verify a 6-digit TOTP code (ASCII, NUL-terminated) against
 * T-1, T, and T+1 for the current unix time.
 * Returns E_OK on match, E_DENIED otherwise.
 */
err_t totp_verify(const char *candidate);

/*
 * totp_selftest — compute a code for the current T and immediately verify it.
 * Logs PASS/FAIL. Non-fatal; for boot-time confidence only.
 */
err_t totp_selftest(void);

#endif /* VSE_TOTP_H */
