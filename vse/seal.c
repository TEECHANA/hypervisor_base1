/*
 * seal.c — VSE Phase 4: Data Sealing / Unsealing implementation
 *
 * Binds data to platform state using HMAC. The seal key is derived from a
 * master key mixed with the current configuration HMAC, so a sealed blob
 * only unseals on a platform whose config_blob_t is unchanged.
 *
 * No dynamic allocation; all buffers are caller-provided or on-stack and
 * scrubbed after use.
 *
 * DESIGN NOTE — confidentiality:
 *   This MVP provides integrity + binding, NOT confidentiality: the payload
 *   is stored in clear inside the blob. To add confidentiality, encrypt
 *   `payload` with AES-GCM using a key derived the same way as seal_key.
 *   The blob layout already reserves the MAC field that GCM would supply.
 */

#include "seal.h"
#include "hmac.h"
#include "config_check.h"
#include "../lib/log/log.h"
#include "../lib/str/string.h"

/* ── Master key (shared with the other VSE modules) ── */
static const u8 _master_key[32] = {
    0xa3, 0x7f, 0x2c, 0x91, 0xd4, 0x58, 0xbe, 0x06,
    0x1e, 0x82, 0x4a, 0xf3, 0x70, 0xc9, 0x55, 0x3d,
    0x08, 0xb1, 0xe7, 0x6f, 0x29, 0xda, 0x44, 0x9c,
    0x87, 0x5e, 0x13, 0xab, 0xfc, 0x60, 0x2b, 0x77,
};

/* Domain-separation label for the seal key KDF. */
static const u8 _seal_label[] = "VSE-SEAL-v1";

static bool _initialized = false;

/* ── Internal: compute the current platform config HMAC (the bind tag) ──
 *
 * Uses config_check_get_blob() so sealing is bound to the SAME config that
 * Phase 1 verifies. If the platform config changes, this tag changes, the
 * seal key changes, and old blobs stop unsealing.
 */
static err_t _platform_bind_tag(u8 tag_out[HMAC_SIZE])
{
    config_blob_t blob;
    config_check_get_blob(&blob);
    return hmac_sha256(_master_key, (u32)sizeof(_master_key),
                       (const u8 *)&blob, (u64)sizeof(blob), tag_out);
}

/* ── Internal: derive the seal key from master key + bind tag ──
 *
 *   seal_key = HMAC(master_key, label || bind_tag)
 */
static err_t _derive_seal_key(const u8 bind_tag[HMAC_SIZE],
                              u8 key_out[HMAC_SIZE])
{
    /* message = label (without NUL) || bind_tag */
    u8 msg[sizeof(_seal_label) - 1u + HMAC_SIZE];
    u32 label_len = (u32)(sizeof(_seal_label) - 1u);

    memcpy(msg, _seal_label, label_len);
    memcpy(msg + label_len, bind_tag, HMAC_SIZE);

    err_t e = hmac_sha256(_master_key, (u32)sizeof(_master_key),
                          msg, (u64)(label_len + HMAC_SIZE), key_out);
    memset(msg, 0, sizeof(msg));
    return e;
}

/* ── Internal: compute the blob MAC over magic||len||bind_tag||payload ── */
static err_t _blob_mac(const u8 seal_key[HMAC_SIZE],
                       u32 magic, u32 payload_len,
                       const u8 bind_tag[HMAC_SIZE],
                       const u8 *payload, u32 payload_cap,
                       u8 mac_out[HMAC_SIZE])
{
    hmac_ctx_t ctx;
    err_t e = hmac_init(&ctx, seal_key, HMAC_SIZE);
    if (FAIL(e)) return e;

    u8 hdr[8];
    hdr[0] = (u8)(magic >> 24); hdr[1] = (u8)(magic >> 16);
    hdr[2] = (u8)(magic >> 8);  hdr[3] = (u8)(magic);
    hdr[4] = (u8)(payload_len >> 24); hdr[5] = (u8)(payload_len >> 16);
    hdr[6] = (u8)(payload_len >> 8);  hdr[7] = (u8)(payload_len);

    hmac_update(&ctx, hdr, sizeof(hdr));
    hmac_update(&ctx, bind_tag, HMAC_SIZE);
    /* MAC the full fixed payload region (incl. zero padding) for a fixed,
     * length-independent layout. payload_cap is always SEAL_MAX_PAYLOAD. */
    hmac_update(&ctx, payload, payload_cap);
    hmac_final(&ctx, mac_out);
    return E_OK;
}

/* ── seal_init ── */
err_t seal_init(void)
{
    /* Verify we can compute a bind tag now (config is available). */
    u8 tag[HMAC_SIZE];
    err_t e = _platform_bind_tag(tag);
    memset(tag, 0, sizeof(tag));
    if (FAIL(e)) {
        LOG_ERROR("VSE Phase 4: seal_init failed to compute bind tag");
        return e;
    }
    _initialized = true;
    LOG_INFO("VSE Phase 4: sealing service ready (blob=%u bytes, max payload=%u)",
             SEALED_BLOB_SIZE, SEAL_MAX_PAYLOAD);
    return E_OK;
}

/* ── vse_seal ── */
err_t vse_seal(const u8 *in, u32 in_len, sealed_blob_t *out)
{
    if (!in || !out)            return E_INVAL;
    if (in_len > SEAL_MAX_PAYLOAD) return E_INVAL;
    if (!_initialized)          return E_INVAL;

    u8 bind_tag[HMAC_SIZE];
    u8 seal_key[HMAC_SIZE];
    err_t e;

    e = _platform_bind_tag(bind_tag);
    if (FAIL(e)) return e;
    e = _derive_seal_key(bind_tag, seal_key);
    if (FAIL(e)) { memset(bind_tag, 0, sizeof(bind_tag)); return e; }

    /* Assemble blob */
    memset(out, 0, sizeof(*out));
    out->magic       = SEAL_MAGIC;
    out->payload_len = in_len;
    memcpy(out->bind_tag, bind_tag, HMAC_SIZE);
    memcpy(out->payload, in, in_len);   /* rest already zero from memset */

    e = _blob_mac(seal_key, out->magic, out->payload_len, out->bind_tag,
                  out->payload, SEAL_MAX_PAYLOAD, out->mac);

    /* scrub secrets */
    memset(seal_key, 0, sizeof(seal_key));
    memset(bind_tag, 0, sizeof(bind_tag));

    if (FAIL(e)) {
        memset(out, 0, sizeof(*out));
        return e;
    }
    return E_OK;
}

/* ── vse_unseal ── */
err_t vse_unseal(const sealed_blob_t *blob, u8 *out, u32 *io_len)
{
    if (!blob || !out || !io_len) return E_INVAL;
    if (!_initialized)            return E_INVAL;
    if (blob->magic != SEAL_MAGIC) {
        LOG_WARN("VSE Phase 4: unseal rejected — bad magic 0x%x", blob->magic);
        return E_INVAL;
    }
    if (blob->payload_len > SEAL_MAX_PAYLOAD) {
        LOG_WARN("VSE Phase 4: unseal rejected — payload_len too large");
        return E_INVAL;
    }

    u8 bind_tag[HMAC_SIZE];
    u8 seal_key[HMAC_SIZE];
    u8 expect_mac[HMAC_SIZE];
    err_t e;

    /* Derive seal key from CURRENT platform state — this is the binding. */
    e = _platform_bind_tag(bind_tag);
    if (FAIL(e)) return e;
    e = _derive_seal_key(bind_tag, seal_key);
    if (FAIL(e)) { memset(bind_tag, 0, sizeof(bind_tag)); return e; }

    /* Recompute MAC over the blob's stored fields. */
    e = _blob_mac(seal_key, blob->magic, blob->payload_len, blob->bind_tag,
                  blob->payload, SEAL_MAX_PAYLOAD, expect_mac);

    memset(seal_key, 0, sizeof(seal_key));
    memset(bind_tag, 0, sizeof(bind_tag));
    if (FAIL(e)) return e;

    /* Constant-time compare. */
    if (!hmac_verify(expect_mac, blob->mac, HMAC_SIZE)) {
        memset(expect_mac, 0, sizeof(expect_mac));
        LOG_WARN("VSE Phase 4: unseal DENIED — MAC mismatch "
                 "(tampered or platform state changed)");
        return E_INVAL;
    }
    memset(expect_mac, 0, sizeof(expect_mac));

    /* Authenticated — return payload. */
    if (*io_len < blob->payload_len) {
        *io_len = blob->payload_len;   /* tell caller required size */
        return E_INVAL;
    }
    memcpy(out, blob->payload, blob->payload_len);
    *io_len = blob->payload_len;
    return E_OK;
}

/* ── seal_selftest ── */
err_t seal_selftest(void)
{
    static const u8 secret[] = "VSE-PHASE4-SEAL-SELFTEST-SECRET";
    const u32 secret_len = (u32)(sizeof(secret) - 1u);

    sealed_blob_t blob;
    u8  recovered[SEAL_MAX_PAYLOAD];
    u32 rlen;
    err_t e;

    LOG_INFO("VSE Phase 4: running seal self-test...");

    /* 1. Seal */
    e = vse_seal(secret, secret_len, &blob);
    if (FAIL(e)) {
        LOG_ERROR("VSE Phase 4: self-test FAILED at seal (err=%d)", (int)e);
        return e;
    }

    /* 2. Unseal — should succeed and match */
    rlen = sizeof(recovered);
    e = vse_unseal(&blob, recovered, &rlen);
    if (FAIL(e)) {
        LOG_ERROR("VSE Phase 4: self-test FAILED at unseal (err=%d)", (int)e);
        return e;
    }
    if (rlen != secret_len || memcmp(recovered, secret, secret_len) != 0) {
        LOG_ERROR("VSE Phase 4: self-test FAILED — recovered payload mismatch");
        memset(recovered, 0, sizeof(recovered));
        return E_INVAL;
    }

    /* 3. Tamper test — flip one payload byte; unseal must now be DENIED */
    blob.payload[0] ^= 0xFFu;
    rlen = sizeof(recovered);
    e = vse_unseal(&blob, recovered, &rlen);
    memset(recovered, 0, sizeof(recovered));
    if (OK(e)) {
        LOG_ERROR("VSE Phase 4: self-test FAILED — tampered blob unsealed!");
        return E_INVAL;
    }

    LOG_INFO("VSE Phase 4: seal self-test PASSED "
             "(round-trip ok, tamper correctly denied)");
    return E_OK;
}
