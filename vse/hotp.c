/*
 * hotp.c — VSE HOTP code primitive (RFC 4226 dynamic truncation)
 *
 * Provides hotp_compute(): HMAC-SHA256(secret, counter) -> N-digit code. This is
 * the shared RFC 4226 primitive that totp.c layers RFC 6238 time-stepping on top
 * of (a time step T = unix_now / step replaces the event counter). The old
 * counter-based HOTP login path (hotp_init/hotp_verify + a sealed persistent
 * counter) was superseded by TOTP and has been removed; only the code-derivation
 * primitive remains.
 *
 * No libc, no dynamic allocation. All crypto via the in-tree HMAC-SHA256.
 */

#include "hotp.h"
#include "hmac.h"
#include "keystore.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/*
 * Shared HOTP secret. No longer a hard-coded blob: it is derived at runtime
 * from the VSE master key via vse_derive_secret() (see keystore.h). On real
 * hardware the master key comes from the per-device OTP fuse, so this secret is
 * automatically device-unique. In the QEMU/dev build it derives from the dev
 * master key — scripts/totp_gen.py mirrors the same derivation so login stays
 * testable. The plaintext secret never appears in source.
 */
#define HOTP_SECRET_LABEL  "vse-hotp-secret-v1"
#define HOTP_SECRET_LEN    20u

static u8   _hotp_secret[HOTP_SECRET_LEN];
static bool _secret_ready = false;

/* Derive the HOTP secret from the master key on first use (idempotent). */
static err_t _ensure_secret(void)
{
    if (_secret_ready) return E_OK;
    err_t e = vse_derive_secret(HOTP_SECRET_LABEL, _hotp_secret, sizeof(_hotp_secret));
    if (FAIL(e)) {
        LOG_ERROR("HOTP: secret derivation failed (err=%d)", (int)e);
        return e;
    }
    _secret_ready = true;
    return E_OK;
}

/* ── helpers ───────────────────────────────────────────────────────────── */

static void _u64_be(u64 v, u8 out[8])
{
    for (int i = 7; i >= 0; --i) { out[i] = (u8)(v & 0xff); v >>= 8; }
}

static u32 _pow10(u32 n)
{
    u32 r = 1;
    while (n--) r *= 10u;
    return r;
}

/* ── public API ────────────────────────────────────────────────────────── */

err_t hotp_compute(u64 counter, char *out, u32 out_sz)
{
    if (!out || out_sz < (HOTP_DIGITS + 1u)) return E_INVAL;

    err_t e = _ensure_secret();
    if (FAIL(e)) return e;

    u8 ctr[8];
    u8 mac[HMAC_SIZE];
    _u64_be(counter, ctr);

    e = hmac_sha256(_hotp_secret, (u32)sizeof(_hotp_secret),
                    ctr, sizeof(ctr), mac);
    if (FAIL(e)) return e;

    /* RFC 4226 dynamic truncation. */
    u32 off = mac[HMAC_SIZE - 1] & 0x0fu;
    u32 bin = ((u32)(mac[off]     & 0x7f) << 24)
            | ((u32)(mac[off + 1] & 0xff) << 16)
            | ((u32)(mac[off + 2] & 0xff) << 8)
            |  (u32)(mac[off + 3] & 0xff);

    u32 code = bin % _pow10(HOTP_DIGITS);

    /* Zero-padded decimal, fixed width. */
    for (int i = (int)HOTP_DIGITS - 1; i >= 0; --i) {
        out[i] = (char)('0' + (code % 10u));
        code /= 10u;
    }
    out[HOTP_DIGITS] = '\0';

    memset(mac, 0, sizeof(mac));
    return E_OK;
}
