/*
 * hmac.h — HMAC-SHA256 for EL2 hypervisor security layer
 *
 * Self-contained: no libc, no external dependencies.
 * All computation happens in EL2 address space —
 * no guest VM can reach this code or the master key.
 *
 * Usage:
 *   u8 mac[HMAC_SIZE];
 *   hmac_sha256(key, key_len, msg, msg_len, mac);
 *
 * For incremental (multi-part) messages:
 *   hmac_ctx_t ctx;
 *   hmac_init(&ctx, key, key_len);
 *   hmac_update(&ctx, part1, len1);
 *   hmac_update(&ctx, part2, len2);
 *   hmac_final(&ctx, mac);
 */

#ifndef VSE_HMAC_H
#define VSE_HMAC_H

#include "../include/types.h"
#include "../include/error.h"

/* Output size of HMAC-SHA256 in bytes */
#define HMAC_SIZE       32u
#define SHA256_BLOCK    64u
#define SHA256_DIGEST   32u

/* SHA-256 internal state */
typedef struct {
    u32 state[8];        /* H0..H7                          */
    u64 bit_count;       /* total bits processed            */
    u8  buf[SHA256_BLOCK]; /* partial block buffer           */
    u32 buf_len;         /* bytes currently in buf          */
} sha256_ctx_t;

/* HMAC context — holds inner and outer SHA-256 states */
typedef struct {
    sha256_ctx_t inner;
    sha256_ctx_t outer;
} hmac_ctx_t;

/* ── SHA-256 raw API (needed by config_check to hash config blobs) ── */
void sha256_init  (sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const u8 *data, u64 len);
void sha256_final (sha256_ctx_t *ctx, u8 digest[SHA256_DIGEST]);

/* ── HMAC-SHA256 streaming API ── */
err_t hmac_init  (hmac_ctx_t *ctx, const u8 *key, u32 key_len);
void  hmac_update(hmac_ctx_t *ctx, const u8 *data, u64 len);
void  hmac_final (hmac_ctx_t *ctx, u8 mac[HMAC_SIZE]);

/* ── HMAC-SHA256 single-shot API ── */
err_t hmac_sha256(const u8 *key,  u32 key_len,
                  const u8 *msg,  u64 msg_len,
                  u8  mac[HMAC_SIZE]);

/* ── Constant-time comparison (prevents timing side-channels) ── */
bool hmac_verify(const u8 *expected, const u8 *actual, u32 len);

#endif /* VSE_HMAC_H */
