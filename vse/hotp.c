/*
 * hotp.c — VSE HOTP implementation (see hotp.h for design rationale)
 *
 * No libc, no dynamic allocation. All crypto via the in-tree HMAC-SHA256.
 */

#include "hotp.h"
#include "hmac.h"
#include "seal.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/*
 * Shared HOTP secret. In a real deployment this is provisioned per-device and
 * NOT committed to source — bake it in at build time (e.g. injected by
 * seal_wrapper.sh) or derive it from a per-unit fuse. This default exists so
 * the subsystem self-tests and so scripts/hotp_gen.py has a matching value for
 * development. Treat it as non-secret until provisioning replaces it.
 */
static const u8 _hotp_secret[20] = {
    0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x21, 0xde, 0xad,
    0xbe, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
    0xde, 0xf0, 0x11, 0x22,
};

/*
 * Fixed reserved RAM region for the sealed counter blob.
 *
 * Chosen to sit just below the shared-memory pool base (SHMEM_POOL_PA =
 * 0xA0000000), in a page the guests never map. One 4 KiB page is ample for a
 * sealed_blob_t. On QEMU virt this RAM exists but is volatile; see
 * TODO(persistence).
 */
#define HOTP_BLOB_PA   0x9FFFF000ULL

/* In-memory authoritative counter (mirrors the sealed blob). */
static u64  _counter      = 0;
static bool _initialized  = false;

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

/* Persist _counter into the sealed blob region. */
static err_t _save_counter(void)
{
    sealed_blob_t *blob = (sealed_blob_t *)(uintptr_t)HOTP_BLOB_PA;
    u8 buf[8];
    _u64_be(_counter, buf);
    err_t e = vse_seal(buf, sizeof(buf), blob);
    memset(buf, 0, sizeof(buf));
    return e;
}

/* Load _counter from the sealed blob region; E_NOTFOUND if no valid blob. */
static err_t _load_counter(void)
{
    sealed_blob_t *blob = (sealed_blob_t *)(uintptr_t)HOTP_BLOB_PA;
    u8 buf[8];
    u32 len = sizeof(buf);
    err_t e = vse_unseal(blob, buf, &len);
    if (FAIL(e) || len != sizeof(buf)) { memset(buf, 0, sizeof(buf)); return E_NOTFOUND; }
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | buf[i];
    _counter = v;
    memset(buf, 0, sizeof(buf));
    return E_OK;
}

/* ── public API ────────────────────────────────────────────────────────── */

err_t hotp_compute(u64 counter, char *out, u32 out_sz)
{
    if (!out || out_sz < (HOTP_DIGITS + 1u)) return E_INVAL;

    u8 ctr[8];
    u8 mac[HMAC_SIZE];
    _u64_be(counter, ctr);

    err_t e = hmac_sha256(_hotp_secret, (u32)sizeof(_hotp_secret),
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

err_t hotp_init(void)
{
    if (_initialized) return E_OK;

    if (OK(_load_counter())) {
        LOG_INFO("HOTP: counter restored from sealed store (c=%lu)", _counter);
    } else {
        _counter = 0;
        LOG_WARN("HOTP: no valid sealed counter; initializing to 0");
        /* Seal the fresh counter so subsequent unseals succeed this boot. */
        if (FAIL(_save_counter()))
            LOG_WARN("HOTP: initial counter seal failed");
    }
    _initialized = true;
    return E_OK;
}

err_t hotp_verify(const char *candidate)
{
    if (!_initialized) return E_DENIED;
    if (!candidate)    return E_INVAL;

    char expect[HOTP_DIGITS + 1];

    /* Try current counter and the look-ahead window. */
    for (u32 skip = 0; skip <= HOTP_LOOKAHEAD; ++skip) {
        if (FAIL(hotp_compute(_counter + skip, expect, sizeof(expect))))
            return E_GENERIC;

        /* Constant-time compare over the fixed digit width. */
        if (hmac_verify((const u8 *)expect, (const u8 *)candidate, HOTP_DIGITS)) {
            /* Match: advance past the consumed counter, persist, resync. */
            _counter = _counter + skip + 1u;
            err_t e = _save_counter();
            if (FAIL(e)) LOG_WARN("HOTP: counter advanced but re-seal failed");
            memset(expect, 0, sizeof(expect));
            return E_OK;
        }
    }
    memset(expect, 0, sizeof(expect));
    return E_DENIED;
}

err_t hotp_selftest(void)
{
    char code[HOTP_DIGITS + 1];
    u64  save = _counter;
    bool save_init = _initialized;

    _initialized = true;       /* allow verify during the test */
    _counter = 0;

    err_t e = hotp_compute(0, code, sizeof(code));
    if (FAIL(e)) { LOG_ERROR("HOTP selftest: compute failed"); goto done; }

    /* The freshly computed code for counter 0 must verify (skip=0). */
    if (OK(hotp_verify(code))) {
        LOG_INFO("HOTP selftest: PASS (c0 code=%s, counter now %lu)", code, _counter);
        e = E_OK;
    } else {
        LOG_ERROR("HOTP selftest: FAIL (code %s did not verify)", code);
        e = E_GENERIC;
    }

done:
    memset(code, 0, sizeof(code));
    _counter = save;
    _initialized = save_init;
    return e;
}

/*
 * TODO(persistence): HOTP_BLOB_PA is volatile RAM on QEMU virt. To make the
 * counter survive power-off, back this region with a non-volatile store:
 *   - QEMU: add a -drive if=pflash and an MMIO CFI-flash read/write driver.
 *   - rpi4: write to a reserved offset on the SD/eMMC.
 *   - s32g: use the on-chip data flash / NVM.
 * The sealing logic above is already correct and tamper-evident; only the
 * backing store needs to change. No change to hotp_verify/hotp_init required.
 */
