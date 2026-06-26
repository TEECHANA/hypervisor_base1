/*
 * seal.h — VSE Phase 4: Data Sealing / Unsealing
 *
 * Sealing binds a small blob of data to the current platform state so that
 * it can only be recovered (unsealed) on a hypervisor whose configuration
 * is unchanged. If the platform config changes (config_blob_t differs), the
 * sealed blob no longer unseals — it is cryptographically bound.
 *
 * This is the mechanism a VSE uses to protect secrets at rest: a guest's
 * key material, a trust anchor, a license token, etc. can be sealed by the
 * hypervisor and stored in untrusted memory, yet only the genuine,
 * unmodified platform can open them.
 *
 * Threat model / what this provides:
 *   - INTEGRITY: tampering with the sealed blob is detected (HMAC).
 *   - BINDING:   the blob only opens when the platform state matches the
 *                state at seal time (config HMAC is mixed into the key).
 *   - It does NOT provide confidentiality on its own (the payload is stored
 *     in the clear inside the blob). For confidentiality, add AES-GCM in a
 *     later phase — see the design note in seal.c.
 *
 * Construction (HMAC-based authenticated binding):
 *   seal_key   = HMAC(master_key, "VSE-SEAL-v1" || config_hmac)
 *   blob.mac   = HMAC(seal_key,   magic || len || pcr || payload)
 * Unseal recomputes seal_key from the CURRENT platform state; if the
 * platform changed, seal_key differs, the MAC check fails, unseal is denied.
 */

#ifndef VSE_SEAL_H
#define VSE_SEAL_H

#include "../include/types.h"
#include "../include/error.h"
#include "hmac.h"

/* Maximum payload that can be sealed in one blob (bytes). */
#define SEAL_MAX_PAYLOAD   256u

/* Magic marker identifying a VSE sealed blob ("VSE4"). */
#define SEAL_MAGIC         0x56534534u

/*
 * Sealed blob layout (all fixed-size, no dynamic alloc):
 *
 *   +----------------+----------------------------------------------+
 *   | field          | size                                         |
 *   +----------------+----------------------------------------------+
 *   | magic          | 4   (SEAL_MAGIC)                             |
 *   | payload_len    | 4   (actual bytes of payload, <= MAX)        |
 *   | bind_tag       | 32  (config HMAC snapshot at seal time)      |
 *   | mac            | 32  (HMAC over magic||len||bind_tag||payload)|
 *   | payload        | SEAL_MAX_PAYLOAD (fixed; only payload_len    |
 *   |                |     bytes meaningful, rest zero-padded)      |
 *   +----------------+----------------------------------------------+
 */
typedef struct __packed {
    u32 magic;
    u32 payload_len;
    u8  bind_tag[HMAC_SIZE];
    u8  mac[HMAC_SIZE];
    u8  payload[SEAL_MAX_PAYLOAD];
} sealed_blob_t;

#define SEALED_BLOB_SIZE  ((u32)sizeof(sealed_blob_t))

/* ── Lifecycle ── */
/*
 * seal_init — derive the platform-bound seal key from current config.
 * Call once at boot AFTER config_check_init() (so config is verified)
 * and after trust_init(). Idempotent.
 */
err_t seal_init(void);

/* ── Core API ── */
/*
 * vse_seal — bind `in_len` bytes of `in` into `out`.
 *   out must point to a sealed_blob_t (SEALED_BLOB_SIZE bytes).
 * Returns E_OK on success, E_INVAL on bad args / oversize payload.
 */
err_t vse_seal(const u8 *in, u32 in_len, sealed_blob_t *out);

/*
 * vse_unseal — verify and recover a sealed blob.
 *   Recomputes the seal key from CURRENT platform state and checks the MAC.
 *   On success copies up to *io_len bytes into `out`, sets *io_len to the
 *   real payload length.
 * Returns:
 *   E_OK       — verified, payload recovered
 *   E_INVAL    — bad args, bad magic, or buffer too small
 *   E_PERM-ish — MAC mismatch (tampered or platform changed) → E_INVAL here,
 *                logged distinctly so the cause is visible.
 */
err_t vse_unseal(const sealed_blob_t *blob, u8 *out, u32 *io_len);

/* ── Diagnostics / self-test ── */
/*
 * seal_selftest — seal a known string and unseal it back, verifying the
 * round trip. Also verifies that a tampered blob fails to unseal. Logs
 * the result. Safe to call at boot to prove the subsystem works.
 */
err_t seal_selftest(void);

#endif /* VSE_SEAL_H */
