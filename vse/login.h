/*
 * login.h — VSE operator login (Activity Tracker Item 1)
 *
 * Two-factor operator authentication at hypervisor boot:
 *   1. A password, verified against a stored SHA-256 hash (never plaintext).
 *   2. A TOTP one-time code (vse/totp.c, RFC 6238) using lib/rtc as the
 *      time source (ARM generic timer + provisioned epoch — no hardware RTC).
 *
 * Called from boot/main.c after the VSE integrity/trust/seal phases, before
 * the scheduler starts — so no guest runs until the operator authenticates,
 * and the platform proving its own integrity comes first.
 *
 * Failure handling: after LOGIN_MAX_TRIES failed attempts the function returns
 * E_DENIED; the caller decides policy (panic / hold). It does not loop forever.
 */

#ifndef VSE_LOGIN_H
#define VSE_LOGIN_H

#include "../include/types.h"
#include "../include/error.h"

#define LOGIN_MAX_TRIES   3u
#define LOGIN_PW_MAX      64u

/*
 * login_authenticate — run the interactive 2FA prompt on the console UART.
 * Returns E_OK on success, E_DENIED after LOGIN_MAX_TRIES failures.
 * Requires uart_init() done and hotp_init() done (so the counter is loaded).
 */
err_t login_authenticate(void);

#endif /* VSE_LOGIN_H */
