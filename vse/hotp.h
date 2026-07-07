/*
 * hotp.h — VSE: HMAC-based One-Time Password (RFC 4226 style) for operator login
 *
 * Background
 * ----------
 * This platform has NO real-time clock — every time source in the tree is
 * cntpct_el0 (counter ticks since power-on), which is unrelated to wall-clock
 * time. Standard TOTP (RFC 6238) needs synchronized wall-clock time and is
 * therefore impossible here. We use HOTP (RFC 4226): event/counter based, no
 * clock required. Each successful login advances a counter shared between the
 * device and the operator's authenticator.
 *
 * Algorithm (RFC 4226, generalized to SHA-256 per the existing hmac.c):
 *   HS      = HMAC-SHA256(secret, counter_be64)        // 32-byte MAC
 *   offset  = HS[len-1] & 0x0f                          // dynamic truncation
 *   bincode = (HS[offset] & 0x7f)<<24 | HS[offset+1]<<16
 *             | HS[offset+2]<<8 | HS[offset+3]
 *   code    = bincode % 10^digits                       // e.g. 6 digits
 *
 * NOTE on SHA-256 vs SHA-1: RFC 4226 specifies HMAC-SHA1. We deliberately
 * reuse the audited in-tree HMAC-SHA256 (vse/hmac.c) instead of adding a new
 * SHA-1 implementation in the EL2 trust path. Generate codes with the matching
 * host tool scripts/hotp_gen.py (which also uses SHA-256). This is fine because
 * both ends are under our control; it is NOT interoperable with a stock
 * Google-Authenticator HOTP entry left on SHA-1 defaults.
 *
 * Counter persistence
 * -------------------
 * The counter is wrapped with vse_seal()/vse_unseal() (VSE Phase 4) so it is
 * tamper-evident and bound to the verified platform config. The sealed blob
 * lives in a fixed reserved RAM region (see hotp.c). On a platform with real
 * non-volatile storage (pflash / eMMC / s32g NVM) that region should be backed
 * by a persistence driver so the counter survives power-off; until then it is
 * RAM-resident and re-initialized from a known base each cold boot. Sealing
 * provides integrity, not persistence — see hotp.c TODO(persistence).
 */

#ifndef VSE_HOTP_H
#define VSE_HOTP_H

#include "../include/types.h"
#include "../include/error.h"

/* Number of decimal digits in a generated code. RFC 4226 allows 6..8. */
#define HOTP_DIGITS        6u

/* Look-ahead window: if the operator's authenticator is a few steps ahead
 * (e.g. they generated codes that never reached the device), accept any code
 * within the next HOTP_LOOKAHEAD counters and resynchronize. */
#define HOTP_LOOKAHEAD     5u

/*
 * hotp_init — load (unseal) the persisted counter, or initialize it to 0 on
 * first boot / when no valid sealed blob is present. Must be called AFTER
 * seal_init() (VSE Phase 4), since it uses vse_seal/vse_unseal.
 */
err_t hotp_init(void);

/*
 * hotp_compute — compute the HOTP code for an explicit counter value.
 * Output is written as a zero-padded ASCII string of HOTP_DIGITS chars plus
 * NUL into out (out must be >= HOTP_DIGITS+1 bytes). Exposed mainly for the
 * self-test and tooling.
 */
err_t hotp_compute(u64 counter, char *out, u32 out_sz);

/*
 * hotp_verify — check a candidate code (ASCII, HOTP_DIGITS chars) against the
 * current counter and the look-ahead window. On match: advances the stored
 * counter past the matched value, re-seals it, and returns E_OK. On no match:
 * returns E_DENIED and does NOT advance (failure handling/lockout is the
 * caller's job — see login.c).
 */
err_t hotp_verify(const char *candidate);

/*
 * hotp_selftest — compute a code for a fixed counter and verify it round-trips.
 * Logs PASS/FAIL. Non-fatal; for boot-time confidence only.
 */
err_t hotp_selftest(void);

#endif /* VSE_HOTP_H */
