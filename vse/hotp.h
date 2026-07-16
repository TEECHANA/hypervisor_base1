/*
 * hotp.h — VSE: RFC 4226 HOTP code primitive (used by TOTP)
 *
 * hotp_compute() implements the RFC 4226 code derivation:
 *   HS      = HMAC-SHA256(secret, counter_be64)        // 32-byte MAC
 *   offset  = HS[len-1] & 0x0f                          // dynamic truncation
 *   bincode = (HS[offset] & 0x7f)<<24 | HS[offset+1]<<16
 *             | HS[offset+2]<<8 | HS[offset+3]
 *   code    = bincode % 10^digits                       // e.g. 6 digits
 *
 * totp.c (RFC 6238) uses this as its primitive, substituting a time step
 * T = floor(unix_now / step) for the event counter. The old counter-based HOTP
 * login (hotp_init / hotp_verify + a sealed persistent counter) was superseded
 * by TOTP and removed; only the code-derivation primitive remains here.
 *
 * NOTE on SHA-256 vs SHA-1: RFC 4226 specifies HMAC-SHA1. We deliberately
 * reuse the audited in-tree HMAC-SHA256 (vse/hmac.c) instead of adding a new
 * SHA-1 implementation in the EL2 trust path. Generate matching codes with the
 * host tool scripts/totp_gen.py (which also uses SHA-256). This is NOT
 * interoperable with a stock Google-Authenticator entry left on SHA-1 defaults;
 * both ends are under our control.
 */

#ifndef VSE_HOTP_H
#define VSE_HOTP_H

#include "../include/types.h"
#include "../include/error.h"

/* Number of decimal digits in a generated code. RFC 4226 allows 6..8. */
#define HOTP_DIGITS        6u

/*
 * hotp_compute — compute the RFC 4226 code for an explicit counter value.
 * Output is written as a zero-padded ASCII string of HOTP_DIGITS chars plus
 * NUL into out (out must be >= HOTP_DIGITS+1 bytes). totp.c calls this with
 * the time step T = floor(unix_now / TOTP_STEP_S) in place of the counter.
 */
err_t hotp_compute(u64 counter, char *out, u32 out_sz);

#endif /* VSE_HOTP_H */
