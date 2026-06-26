/*
 * hmac.c — HMAC-SHA256 implementation for EL2 security layer
 *
 * Implements FIPS 180-4 SHA-256 and FIPS 198-1 HMAC.
 * Zero external dependencies — all arithmetic is self-contained.
 *
 * Security properties:
 *   - Master key lives only in EL2 .rodata — no guest mapping ever covers it.
 *   - hmac_verify() uses constant-time comparison to prevent timing attacks.
 *   - No dynamic allocation: all state is caller-supplied (stack or static).
 *
 * Integration: called by config_check.c, seal.c, ids_log.c.
 */

#include "hmac.h"
#include "../lib/str/string.h"   /* memset, memcpy */

/* ── SHA-256 round constants (first 32 bits of cube roots of primes) ── */
static const u32 K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* SHA-256 initial hash values (first 32 bits of sqrt of first 8 primes) */
static const u32 H0[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

/* ── Bit rotation helpers ── */
static inline u32 rotr32(u32 x, u32 n) { return (x >> n) | (x << (32u - n)); }

/* ── SHA-256 functions ── */
static inline u32 ch (u32 e, u32 f, u32 g) { return (e & f) ^ (~e & g); }
static inline u32 maj(u32 a, u32 b, u32 c) { return (a & b) ^ (a & c) ^ (b & c); }
static inline u32 ep0(u32 a) { return rotr32(a,2)  ^ rotr32(a,13) ^ rotr32(a,22); }
static inline u32 ep1(u32 e) { return rotr32(e,6)  ^ rotr32(e,11) ^ rotr32(e,25); }
static inline u32 sig0(u32 x){ return rotr32(x,7)  ^ rotr32(x,18) ^ (x >> 3);     }
static inline u32 sig1(u32 x){ return rotr32(x,17) ^ rotr32(x,19) ^ (x >> 10);    }

/* Read big-endian u32 from byte array */
static inline u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16)
         | ((u32)p[2] <<  8) |  (u32)p[3];
}

/* Write big-endian u32 to byte array */
static inline void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >>  8); p[3] = (u8)v;
}

/* ── SHA-256 single block compression ── */
static void sha256_compress(u32 state[8], const u8 block[SHA256_BLOCK])
{
    u32 W[64];
    u32 a, b, c, d, e, f, g, h;
    u32 t1, t2;

    /* Prepare message schedule */
    for (u32 i = 0; i < 16u; i++)
        W[i] = be32(block + i * 4u);
    for (u32 i = 16u; i < 64u; i++)
        W[i] = sig1(W[i-2]) + W[i-7] + sig0(W[i-15]) + W[i-16];

    /* Load state */
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    /* 64 rounds */
    for (u32 i = 0; i < 64u; i++) {
        t1 = h + ep1(e) + ch(e, f, g) + K[i] + W[i];
        t2 = ep0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    /* Add compressed chunk to state */
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* ── SHA-256 public API ── */

void sha256_init(sha256_ctx_t *ctx)
{
    for (u32 i = 0; i < 8u; i++)
        ctx->state[i] = H0[i];
    ctx->bit_count = 0;
    ctx->buf_len   = 0;
}

void sha256_update(sha256_ctx_t *ctx, const u8 *data, u64 len)
{
    ctx->bit_count += len * 8ULL;

    u64 i = 0;
    /* Fill partial buffer first */
    if (ctx->buf_len > 0) {
        u32 space = SHA256_BLOCK - ctx->buf_len;
        u32 take  = (u32)((len < space) ? len : space);
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        i            += take;

        if (ctx->buf_len == SHA256_BLOCK) {
            sha256_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    /* Process full blocks directly from input */
    while (i + SHA256_BLOCK <= len) {
        sha256_compress(ctx->state, data + i);
        i += SHA256_BLOCK;
    }

    /* Store remaining bytes in buffer */
    u64 tail = len - i;
    if (tail > 0) {
        memcpy(ctx->buf, data + i, (u32)tail);
        ctx->buf_len = (u32)tail;
    }
}

void sha256_final(sha256_ctx_t *ctx, u8 digest[SHA256_DIGEST])
{
    u8 pad[SHA256_BLOCK * 2];
    u32 pad_len;

    /* Append 0x80 bit */
    ctx->buf[ctx->buf_len++] = 0x80u;

    /* Padding: need 8 bytes for length at end of block */
    if (ctx->buf_len > 56u) {
        /* Not enough room — pad to end of block, compress, then pad again */
        memset(ctx->buf + ctx->buf_len, 0, SHA256_BLOCK - ctx->buf_len);
        sha256_compress(ctx->state, ctx->buf);
        memset(ctx->buf, 0, 56u);
    } else {
        memset(ctx->buf + ctx->buf_len, 0, 56u - ctx->buf_len);
    }

    /* Append bit length as 64-bit big-endian */
    u64 bits = ctx->bit_count;
    ctx->buf[56] = (u8)(bits >> 56); ctx->buf[57] = (u8)(bits >> 48);
    ctx->buf[58] = (u8)(bits >> 40); ctx->buf[59] = (u8)(bits >> 32);
    ctx->buf[60] = (u8)(bits >> 24); ctx->buf[61] = (u8)(bits >> 16);
    ctx->buf[62] = (u8)(bits >>  8); ctx->buf[63] = (u8)(bits);

    sha256_compress(ctx->state, ctx->buf);

    /* Output digest as big-endian u32 words */
    for (u32 i = 0; i < 8u; i++)
        put_be32(digest + i * 4u, ctx->state[i]);

    UNUSED(pad); UNUSED(pad_len);  /* suppress unused-variable warning */
}

/* ── HMAC-SHA256 implementation (FIPS 198-1) ── */

err_t hmac_init(hmac_ctx_t *ctx, const u8 *key, u32 key_len)
{
    if (!ctx || !key || key_len == 0) return E_INVAL;

    u8 k_ipad[SHA256_BLOCK];
    u8 k_opad[SHA256_BLOCK];
    u8 k_block[SHA256_BLOCK];

    memset(k_block, 0, SHA256_BLOCK);

    /* Keys longer than block size are hashed first */
    if (key_len > SHA256_BLOCK) {
        sha256_ctx_t tmp;
        sha256_init(&tmp);
        sha256_update(&tmp, key, key_len);
        sha256_final(&tmp, k_block);
    } else {
        memcpy(k_block, key, key_len);
    }

    /* Build ipad and opad */
    for (u32 i = 0; i < SHA256_BLOCK; i++) {
        k_ipad[i] = k_block[i] ^ 0x36u;
        k_opad[i] = k_block[i] ^ 0x5Cu;
    }

    /* Inner: H(k_ipad || ...) */
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, k_ipad, SHA256_BLOCK);

    /* Outer: H(k_opad || inner_digest) — start but don't finish yet */
    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, k_opad, SHA256_BLOCK);

    /* Zeroize key material from stack */
    memset(k_ipad,  0, SHA256_BLOCK);
    memset(k_opad,  0, SHA256_BLOCK);
    memset(k_block, 0, SHA256_BLOCK);

    return E_OK;
}

void hmac_update(hmac_ctx_t *ctx, const u8 *data, u64 len)
{
    sha256_update(&ctx->inner, data, len);
}

void hmac_final(hmac_ctx_t *ctx, u8 mac[HMAC_SIZE])
{
    u8 inner_digest[SHA256_DIGEST];

    /* Finish inner hash */
    sha256_final(&ctx->inner, inner_digest);

    /* Feed inner digest into outer hash */
    sha256_update(&ctx->outer, inner_digest, SHA256_DIGEST);
    sha256_final(&ctx->outer, mac);

    /* Zeroize inner digest from stack */
    memset(inner_digest, 0, SHA256_DIGEST);
}

err_t hmac_sha256(const u8 *key, u32 key_len,
                  const u8 *msg, u64 msg_len,
                  u8 mac[HMAC_SIZE])
{
    hmac_ctx_t ctx;
    err_t e = hmac_init(&ctx, key, key_len);
    if (FAIL(e)) return e;

    hmac_update(&ctx, msg, msg_len);
    hmac_final(&ctx, mac);

    memset(&ctx, 0, sizeof(ctx));  /* zeroize HMAC state */
    return E_OK;
}

/*
 * hmac_verify — constant-time comparison to prevent timing side-channels.
 *
 * A naive memcmp returns early on the first mismatch — an attacker can
 * measure the response time to learn how many bytes are correct.
 * This accumulates all differences with OR so every byte is always
 * evaluated regardless of whether earlier bytes matched.
 */
bool hmac_verify(const u8 *expected, const u8 *actual, u32 len)
{
    u8 diff = 0;
    for (u32 i = 0; i < len; i++)
        diff |= expected[i] ^ actual[i];
    return (diff == 0);
}
