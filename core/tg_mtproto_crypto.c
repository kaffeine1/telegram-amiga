/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "tg_mtproto_crypto.h"

#define TG_SHA1_BLOCK_SIZE 64
#define TG_SHA256_BLOCK_SIZE 64
#define TG_SHA512_BLOCK_SIZE 128
#define TG_PBKDF2_SALT_BLOCK_MAX 512

typedef struct tg_sha1_context {
    unsigned long h[5];
    unsigned char block[TG_SHA1_BLOCK_SIZE];
    unsigned long block_used;
    unsigned long length_hi;
    unsigned long length_lo;
} tg_sha1_context;

typedef struct tg_sha256_context {
    unsigned long h[8];
    unsigned char block[TG_SHA256_BLOCK_SIZE];
    unsigned long block_used;
    unsigned long length_hi;
    unsigned long length_lo;
} tg_sha256_context;

typedef struct tg_sha512_context {
    unsigned long long h[8];
    unsigned char block[TG_SHA512_BLOCK_SIZE];
    unsigned long block_used;
    unsigned long long length_hi;
    unsigned long long length_lo;
} tg_sha512_context;

static void (*tg_mtproto_progress_hook)(void) = 0;

void tg_mtproto_set_progress_hook(void (*hook)(void))
{
    tg_mtproto_progress_hook = hook;
}

void tg_mtproto_progress_tick(void)
{
    if (tg_mtproto_progress_hook != 0) {
        tg_mtproto_progress_hook();
    }
}

static unsigned long tg_rotl32(unsigned long value, unsigned long bits)
{
    value &= 0xffffffffUL;
    return ((value << bits) | (value >> (32UL - bits))) & 0xffffffffUL;
}

#if !defined(TG_NO_SELFTEST)
/* Only the reference SHA-256 kept for the self-test still rotates this way. */
static unsigned long tg_rotr32(unsigned long value, unsigned long bits)
{
    value &= 0xffffffffUL;
    return ((value >> bits) | (value << (32UL - bits))) & 0xffffffffUL;
}
#endif

static unsigned long tg_load_be32(const unsigned char *data)
{
    return (((unsigned long)data[0]) << 24) |
           (((unsigned long)data[1]) << 16) |
           (((unsigned long)data[2]) << 8) |
           ((unsigned long)data[3]);
}

static void tg_store_be32(unsigned char *data, unsigned long value)
{
    data[0] = (unsigned char)((value >> 24) & 0xffUL);
    data[1] = (unsigned char)((value >> 16) & 0xffUL);
    data[2] = (unsigned char)((value >> 8) & 0xffUL);
    data[3] = (unsigned char)(value & 0xffUL);
}

static unsigned long long tg_rotr64(unsigned long long value,
                                    unsigned int bits)
{
    return (value >> bits) | (value << (64U - bits));
}

static unsigned long long tg_load_be64(const unsigned char *data)
{
    return (((unsigned long long)data[0]) << 56) |
           (((unsigned long long)data[1]) << 48) |
           (((unsigned long long)data[2]) << 40) |
           (((unsigned long long)data[3]) << 32) |
           (((unsigned long long)data[4]) << 24) |
           (((unsigned long long)data[5]) << 16) |
           (((unsigned long long)data[6]) << 8) |
           ((unsigned long long)data[7]);
}

static void tg_store_be64(unsigned char *data, unsigned long long value)
{
    data[0] = (unsigned char)((value >> 56) & 0xffULL);
    data[1] = (unsigned char)((value >> 48) & 0xffULL);
    data[2] = (unsigned char)((value >> 40) & 0xffULL);
    data[3] = (unsigned char)((value >> 32) & 0xffULL);
    data[4] = (unsigned char)((value >> 24) & 0xffULL);
    data[5] = (unsigned char)((value >> 16) & 0xffULL);
    data[6] = (unsigned char)((value >> 8) & 0xffULL);
    data[7] = (unsigned char)(value & 0xffULL);
}

static void tg_hash_add_length(tg_sha1_context *context, unsigned long data_length)
{
    unsigned long old_lo;
    unsigned long add_hi;
    unsigned long add_lo;

    add_hi = (data_length >> 29) & 0xffffffffUL;
    add_lo = (data_length << 3) & 0xffffffffUL;
    old_lo = context->length_lo;
    context->length_lo = (context->length_lo + add_lo) & 0xffffffffUL;
    context->length_hi = (context->length_hi + add_hi +
                          (context->length_lo < old_lo ? 1UL : 0UL)) &
                         0xffffffffUL;
}

static void tg_hash_add_length_256(tg_sha256_context *context,
                                   unsigned long data_length)
{
    unsigned long old_lo;
    unsigned long add_hi;
    unsigned long add_lo;

    add_hi = (data_length >> 29) & 0xffffffffUL;
    add_lo = (data_length << 3) & 0xffffffffUL;
    old_lo = context->length_lo;
    context->length_lo = (context->length_lo + add_lo) & 0xffffffffUL;
    context->length_hi = (context->length_hi + add_hi +
                          (context->length_lo < old_lo ? 1UL : 0UL)) &
                         0xffffffffUL;
}

static void tg_sha1_transform(tg_sha1_context *context,
                              const unsigned char block[TG_SHA1_BLOCK_SIZE])
{
    unsigned long w[80];
    unsigned long a;
    unsigned long b;
    unsigned long c;
    unsigned long d;
    unsigned long e;
    unsigned long f;
    unsigned long k;
    unsigned long temp;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = tg_load_be32(block + (i * 4));
    }
    for (i = 16; i < 80; ++i) {
        w[i] = tg_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = context->h[0];
    b = context->h[1];
    c = context->h[2];
    d = context->h[3];
    e = context->h[4];

    for (i = 0; i < 80; ++i) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999UL;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1UL;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcUL;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6UL;
        }
        temp = (tg_rotl32(a, 5) + f + e + k + w[i]) & 0xffffffffUL;
        e = d;
        d = c;
        c = tg_rotl32(b, 30);
        b = a;
        a = temp;
    }

    context->h[0] = (context->h[0] + a) & 0xffffffffUL;
    context->h[1] = (context->h[1] + b) & 0xffffffffUL;
    context->h[2] = (context->h[2] + c) & 0xffffffffUL;
    context->h[3] = (context->h[3] + d) & 0xffffffffUL;
    context->h[4] = (context->h[4] + e) & 0xffffffffUL;
}

static void tg_sha1_init(tg_sha1_context *context)
{
    context->h[0] = 0x67452301UL;
    context->h[1] = 0xefcdab89UL;
    context->h[2] = 0x98badcfeUL;
    context->h[3] = 0x10325476UL;
    context->h[4] = 0xc3d2e1f0UL;
    context->block_used = 0;
    context->length_hi = 0;
    context->length_lo = 0;
}

static void tg_sha1_update(tg_sha1_context *context, const unsigned char *data,
                           unsigned long data_length)
{
    unsigned long take;

    tg_hash_add_length(context, data_length);
    while (data_length > 0) {
        take = TG_SHA1_BLOCK_SIZE - context->block_used;
        if (take > data_length) {
            take = data_length;
        }
        memcpy(context->block + context->block_used, data, (size_t)take);
        context->block_used += take;
        data += take;
        data_length -= take;
        if (context->block_used == TG_SHA1_BLOCK_SIZE) {
            tg_sha1_transform(context, context->block);
            context->block_used = 0;
        }
    }
}

static void tg_sha1_final(tg_sha1_context *context,
                          unsigned char digest[TG_MTPROTO_SHA1_LENGTH])
{
    int i;

    context->block[context->block_used++] = 0x80U;
    if (context->block_used > 56UL) {
        while (context->block_used < TG_SHA1_BLOCK_SIZE) {
            context->block[context->block_used++] = 0;
        }
        tg_sha1_transform(context, context->block);
        context->block_used = 0;
    }
    while (context->block_used < 56UL) {
        context->block[context->block_used++] = 0;
    }
    tg_store_be32(context->block + 56, context->length_hi);
    tg_store_be32(context->block + 60, context->length_lo);
    tg_sha1_transform(context, context->block);

    for (i = 0; i < 5; ++i) {
        tg_store_be32(digest + (i * 4), context->h[i]);
    }
}

void tg_mtproto_sha1(const unsigned char *data, unsigned long data_length,
                     unsigned char digest[TG_MTPROTO_SHA1_LENGTH])
{
    tg_sha1_context context;

    tg_sha1_init(&context);
    if (data_length > 0) {
        tg_sha1_update(&context, data, data_length);
    }
    tg_sha1_final(&context, digest);
}

/* SHA-256 on 32-bit words (0.0.95): FIPS 180-4, with the eight working
   variables renamed from round to round instead of moved (eight rounds per
   pass of the loop), rotations the compiler turns into single instructions
   and unsigned int, 32 bits on every lane, so no masks. Every message the
   client receives is hashed whole to check its message key: on a Vampire
   that hash was about half of the 86 ms a 32 KB download part spent in
   decryption. */
typedef unsigned int tg_sha_word;
typedef char tg_sha_word_is_32_bits[(sizeof(tg_sha_word) == 4U) ? 1 : -1];

static const tg_sha_word tg_sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

#define TG_SHA_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define TG_SHA_CH(e, f, g) ((g) ^ ((e) & ((f) ^ (g))))
#define TG_SHA_MAJ(a, b, c) (((a) & (b)) | ((c) & ((a) | (b))))
#define TG_SHA_BS0(a) (TG_SHA_ROTR(a, 2) ^ TG_SHA_ROTR(a, 13) ^ \
                       TG_SHA_ROTR(a, 22))
#define TG_SHA_BS1(e) (TG_SHA_ROTR(e, 6) ^ TG_SHA_ROTR(e, 11) ^ \
                       TG_SHA_ROTR(e, 25))
#define TG_SHA_SS0(x) (TG_SHA_ROTR(x, 7) ^ TG_SHA_ROTR(x, 18) ^ ((x) >> 3))
#define TG_SHA_SS1(x) (TG_SHA_ROTR(x, 17) ^ TG_SHA_ROTR(x, 19) ^ ((x) >> 10))
#define TG_SHA_ROUND(a, b, c, d, e, f, g, h, i) \
    do { \
        tg_sha_word t1_ = (h) + TG_SHA_BS1(e) + TG_SHA_CH(e, f, g) + \
                          tg_sha256_k[i] + w[i]; \
        (d) += t1_; \
        (h) = t1_ + TG_SHA_BS0(a) + TG_SHA_MAJ(a, b, c); \
    } while (0)

static void tg_sha256_transform(tg_sha256_context *context,
                                const unsigned char block[TG_SHA256_BLOCK_SIZE])
{
    tg_sha_word w[64];
    tg_sha_word a;
    tg_sha_word b;
    tg_sha_word c;
    tg_sha_word d;
    tg_sha_word e;
    tg_sha_word f;
    tg_sha_word g;
    tg_sha_word h;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((tg_sha_word)block[i * 4] << 24) |
               ((tg_sha_word)block[i * 4 + 1] << 16) |
               ((tg_sha_word)block[i * 4 + 2] << 8) |
               (tg_sha_word)block[i * 4 + 3];
    }
    for (i = 16; i < 64; ++i) {
        w[i] = TG_SHA_SS1(w[i - 2]) + w[i - 7] + TG_SHA_SS0(w[i - 15]) +
               w[i - 16];
    }
    a = (tg_sha_word)context->h[0];
    b = (tg_sha_word)context->h[1];
    c = (tg_sha_word)context->h[2];
    d = (tg_sha_word)context->h[3];
    e = (tg_sha_word)context->h[4];
    f = (tg_sha_word)context->h[5];
    g = (tg_sha_word)context->h[6];
    h = (tg_sha_word)context->h[7];
    for (i = 0; i < 64; i += 8) {
        TG_SHA_ROUND(a, b, c, d, e, f, g, h, i);
        TG_SHA_ROUND(h, a, b, c, d, e, f, g, i + 1);
        TG_SHA_ROUND(g, h, a, b, c, d, e, f, i + 2);
        TG_SHA_ROUND(f, g, h, a, b, c, d, e, i + 3);
        TG_SHA_ROUND(e, f, g, h, a, b, c, d, i + 4);
        TG_SHA_ROUND(d, e, f, g, h, a, b, c, i + 5);
        TG_SHA_ROUND(c, d, e, f, g, h, a, b, i + 6);
        TG_SHA_ROUND(b, c, d, e, f, g, h, a, i + 7);
    }
    context->h[0] = (unsigned long)(tg_sha_word)(context->h[0] + a);
    context->h[1] = (unsigned long)(tg_sha_word)(context->h[1] + b);
    context->h[2] = (unsigned long)(tg_sha_word)(context->h[2] + c);
    context->h[3] = (unsigned long)(tg_sha_word)(context->h[3] + d);
    context->h[4] = (unsigned long)(tg_sha_word)(context->h[4] + e);
    context->h[5] = (unsigned long)(tg_sha_word)(context->h[5] + f);
    context->h[6] = (unsigned long)(tg_sha_word)(context->h[6] + g);
    context->h[7] = (unsigned long)(tg_sha_word)(context->h[7] + h);
}

#if !defined(TG_NO_SELFTEST)
/* The SHA-256 transform this client used up to 0.0.94, kept as the
   reference the self-test compares the word form against and the
   benchmark times it against. Not in the release binaries. */
static void tg_sha256_transform_ref(tg_sha256_context *context,
                                    const unsigned char block[TG_SHA256_BLOCK_SIZE])
{
    unsigned long w[64];
    unsigned long a;
    unsigned long b;
    unsigned long c;
    unsigned long d;
    unsigned long e;
    unsigned long f;
    unsigned long g;
    unsigned long h;
    unsigned long s0;
    unsigned long s1;
    unsigned long ch;
    unsigned long maj;
    unsigned long temp1;
    unsigned long temp2;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = tg_load_be32(block + (i * 4));
    }
    for (i = 16; i < 64; ++i) {
        s0 = tg_rotr32(w[i - 15], 7) ^ tg_rotr32(w[i - 15], 18) ^
             (w[i - 15] >> 3);
        s1 = tg_rotr32(w[i - 2], 17) ^ tg_rotr32(w[i - 2], 19) ^
             (w[i - 2] >> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & 0xffffffffUL;
    }

    a = context->h[0];
    b = context->h[1];
    c = context->h[2];
    d = context->h[3];
    e = context->h[4];
    f = context->h[5];
    g = context->h[6];
    h = context->h[7];

    for (i = 0; i < 64; ++i) {
        s1 = tg_rotr32(e, 6) ^ tg_rotr32(e, 11) ^ tg_rotr32(e, 25);
        ch = (e & f) ^ ((~e) & g);
        temp1 = (h + s1 + ch + (unsigned long)tg_sha256_k[i] + w[i]) & 0xffffffffUL;
        s0 = tg_rotr32(a, 2) ^ tg_rotr32(a, 13) ^ tg_rotr32(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        temp2 = (s0 + maj) & 0xffffffffUL;
        h = g;
        g = f;
        f = e;
        e = (d + temp1) & 0xffffffffUL;
        d = c;
        c = b;
        b = a;
        a = (temp1 + temp2) & 0xffffffffUL;
    }

    context->h[0] = (context->h[0] + a) & 0xffffffffUL;
    context->h[1] = (context->h[1] + b) & 0xffffffffUL;
    context->h[2] = (context->h[2] + c) & 0xffffffffUL;
    context->h[3] = (context->h[3] + d) & 0xffffffffUL;
    context->h[4] = (context->h[4] + e) & 0xffffffffUL;
    context->h[5] = (context->h[5] + f) & 0xffffffffUL;
    context->h[6] = (context->h[6] + g) & 0xffffffffUL;
    context->h[7] = (context->h[7] + h) & 0xffffffffUL;
}
#endif /* !TG_NO_SELFTEST */

static void tg_sha256_init(tg_sha256_context *context)
{
    context->h[0] = 0x6a09e667UL;
    context->h[1] = 0xbb67ae85UL;
    context->h[2] = 0x3c6ef372UL;
    context->h[3] = 0xa54ff53aUL;
    context->h[4] = 0x510e527fUL;
    context->h[5] = 0x9b05688cUL;
    context->h[6] = 0x1f83d9abUL;
    context->h[7] = 0x5be0cd19UL;
    context->block_used = 0;
    context->length_hi = 0;
    context->length_lo = 0;
}

static void tg_sha256_update(tg_sha256_context *context,
                             const unsigned char *data,
                             unsigned long data_length)
{
    unsigned long take;

    tg_hash_add_length_256(context, data_length);
    while (data_length > 0) {
        if (context->block_used == 0 && data_length >= TG_SHA256_BLOCK_SIZE) {
            /* a whole block in place: no copy through the context */
            tg_sha256_transform(context, data);
            data += TG_SHA256_BLOCK_SIZE;
            data_length -= TG_SHA256_BLOCK_SIZE;
            continue;
        }
        take = TG_SHA256_BLOCK_SIZE - context->block_used;
        if (take > data_length) {
            take = data_length;
        }
        memcpy(context->block + context->block_used, data, (size_t)take);
        context->block_used += take;
        data += take;
        data_length -= take;
        if (context->block_used == TG_SHA256_BLOCK_SIZE) {
            tg_sha256_transform(context, context->block);
            context->block_used = 0;
        }
    }
}

static void tg_sha256_final(tg_sha256_context *context,
                            unsigned char digest[TG_MTPROTO_SHA256_LENGTH])
{
    int i;

    context->block[context->block_used++] = 0x80U;
    if (context->block_used > 56UL) {
        while (context->block_used < TG_SHA256_BLOCK_SIZE) {
            context->block[context->block_used++] = 0;
        }
        tg_sha256_transform(context, context->block);
        context->block_used = 0;
    }
    while (context->block_used < 56UL) {
        context->block[context->block_used++] = 0;
    }
    tg_store_be32(context->block + 56, context->length_hi);
    tg_store_be32(context->block + 60, context->length_lo);
    tg_sha256_transform(context, context->block);

    for (i = 0; i < 8; ++i) {
        tg_store_be32(digest + (i * 4), context->h[i]);
    }
}

void tg_mtproto_sha256(const unsigned char *data, unsigned long data_length,
                       unsigned char digest[TG_MTPROTO_SHA256_LENGTH])
{
    tg_sha256_context context;

    tg_sha256_init(&context);
    if (data_length > 0) {
        tg_sha256_update(&context, data, data_length);
    }
    tg_sha256_final(&context, digest);
}

static void tg_hash_add_length_512(tg_sha512_context *context,
                                   unsigned long data_length)
{
    unsigned long long old_lo;

    old_lo = context->length_lo;
    context->length_lo += ((unsigned long long)data_length) << 3;
    if (context->length_lo < old_lo) {
        ++context->length_hi;
    }
    context->length_hi += ((unsigned long long)data_length) >> 61;
}

static void tg_sha512_transform(tg_sha512_context *context,
                                const unsigned char block[TG_SHA512_BLOCK_SIZE])
{
    static const unsigned long long k[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
        0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
        0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
        0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
        0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
        0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
        0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
        0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
        0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
        0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
        0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
        0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
        0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
        0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
        0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
        0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
        0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
        0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
        0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
        0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
        0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
    };
    unsigned long long w[80];
    unsigned long long a;
    unsigned long long b;
    unsigned long long c;
    unsigned long long d;
    unsigned long long e;
    unsigned long long f;
    unsigned long long g;
    unsigned long long h;
    unsigned long long s0;
    unsigned long long s1;
    unsigned long long ch;
    unsigned long long maj;
    unsigned long long temp1;
    unsigned long long temp2;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = tg_load_be64(block + (i * 8));
    }
    for (i = 16; i < 80; ++i) {
        s0 = tg_rotr64(w[i - 15], 1U) ^ tg_rotr64(w[i - 15], 8U) ^
             (w[i - 15] >> 7);
        s1 = tg_rotr64(w[i - 2], 19U) ^ tg_rotr64(w[i - 2], 61U) ^
             (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = context->h[0];
    b = context->h[1];
    c = context->h[2];
    d = context->h[3];
    e = context->h[4];
    f = context->h[5];
    g = context->h[6];
    h = context->h[7];

    for (i = 0; i < 80; ++i) {
        s1 = tg_rotr64(e, 14U) ^ tg_rotr64(e, 18U) ^ tg_rotr64(e, 41U);
        ch = (e & f) ^ ((~e) & g);
        temp1 = h + s1 + ch + k[i] + w[i];
        s0 = tg_rotr64(a, 28U) ^ tg_rotr64(a, 34U) ^ tg_rotr64(a, 39U);
        maj = (a & b) ^ (a & c) ^ (b & c);
        temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->h[0] += a;
    context->h[1] += b;
    context->h[2] += c;
    context->h[3] += d;
    context->h[4] += e;
    context->h[5] += f;
    context->h[6] += g;
    context->h[7] += h;
}

static void tg_sha512_init(tg_sha512_context *context)
{
    context->h[0] = 0x6a09e667f3bcc908ULL;
    context->h[1] = 0xbb67ae8584caa73bULL;
    context->h[2] = 0x3c6ef372fe94f82bULL;
    context->h[3] = 0xa54ff53a5f1d36f1ULL;
    context->h[4] = 0x510e527fade682d1ULL;
    context->h[5] = 0x9b05688c2b3e6c1fULL;
    context->h[6] = 0x1f83d9abfb41bd6bULL;
    context->h[7] = 0x5be0cd19137e2179ULL;
    context->block_used = 0;
    context->length_hi = 0ULL;
    context->length_lo = 0ULL;
}

static void tg_sha512_update(tg_sha512_context *context,
                             const unsigned char *data,
                             unsigned long data_length)
{
    unsigned long take;

    tg_hash_add_length_512(context, data_length);
    while (data_length > 0UL) {
        take = TG_SHA512_BLOCK_SIZE - context->block_used;
        if (take > data_length) {
            take = data_length;
        }
        memcpy(context->block + context->block_used, data, (size_t)take);
        context->block_used += take;
        data += take;
        data_length -= take;
        if (context->block_used == TG_SHA512_BLOCK_SIZE) {
            tg_sha512_transform(context, context->block);
            context->block_used = 0;
        }
    }
}

static void tg_sha512_final(tg_sha512_context *context,
                            unsigned char digest[TG_MTPROTO_SHA512_LENGTH])
{
    int i;

    context->block[context->block_used++] = 0x80U;
    if (context->block_used > 112UL) {
        while (context->block_used < TG_SHA512_BLOCK_SIZE) {
            context->block[context->block_used++] = 0;
        }
        tg_sha512_transform(context, context->block);
        context->block_used = 0;
    }
    while (context->block_used < 112UL) {
        context->block[context->block_used++] = 0;
    }
    tg_store_be64(context->block + 112, context->length_hi);
    tg_store_be64(context->block + 120, context->length_lo);
    tg_sha512_transform(context, context->block);

    for (i = 0; i < 8; ++i) {
        tg_store_be64(digest + (i * 8), context->h[i]);
    }
}

void tg_mtproto_sha512(const unsigned char *data, unsigned long data_length,
                       unsigned char digest[TG_MTPROTO_SHA512_LENGTH])
{
    tg_sha512_context context;

    tg_sha512_init(&context);
    if (data_length > 0UL) {
        tg_sha512_update(&context, data, data_length);
    }
    tg_sha512_final(&context, digest);
}

void tg_mtproto_hmac_sha512(const unsigned char *key,
                            unsigned long key_length,
                            const unsigned char *data,
                            unsigned long data_length,
                            unsigned char digest[TG_MTPROTO_SHA512_LENGTH])
{
    unsigned char key_block[TG_SHA512_BLOCK_SIZE];
    unsigned char hashed_key[TG_MTPROTO_SHA512_LENGTH];
    unsigned char inner_pad[TG_SHA512_BLOCK_SIZE];
    unsigned char outer_pad[TG_SHA512_BLOCK_SIZE];
    unsigned char inner_digest[TG_MTPROTO_SHA512_LENGTH];
    tg_sha512_context context;
    unsigned int i;

    memset(key_block, 0, sizeof(key_block));
    if (key_length > TG_SHA512_BLOCK_SIZE) {
        tg_mtproto_sha512(key, key_length, hashed_key);
        memcpy(key_block, hashed_key, sizeof(hashed_key));
    } else if (key_length > 0UL) {
        memcpy(key_block, key, (size_t)key_length);
    }
    for (i = 0U; i < TG_SHA512_BLOCK_SIZE; ++i) {
        inner_pad[i] = (unsigned char)(key_block[i] ^ 0x36U);
        outer_pad[i] = (unsigned char)(key_block[i] ^ 0x5cU);
    }

    tg_sha512_init(&context);
    tg_sha512_update(&context, inner_pad, sizeof(inner_pad));
    if (data_length > 0UL) {
        tg_sha512_update(&context, data, data_length);
    }
    tg_sha512_final(&context, inner_digest);

    tg_sha512_init(&context);
    tg_sha512_update(&context, outer_pad, sizeof(outer_pad));
    tg_sha512_update(&context, inner_digest, sizeof(inner_digest));
    tg_sha512_final(&context, digest);
}

int tg_mtproto_pbkdf2_hmac_sha512(const unsigned char *password,
                                  unsigned long password_length,
                                  const unsigned char *salt,
                                  unsigned long salt_length,
                                  unsigned long iterations,
                                  unsigned char *output,
                                  unsigned long output_length)
{
    unsigned char salt_block[TG_PBKDF2_SALT_BLOCK_MAX];
    unsigned char key_block[TG_SHA512_BLOCK_SIZE];
    unsigned char hashed_key[TG_MTPROTO_SHA512_LENGTH];
    unsigned char pad[TG_SHA512_BLOCK_SIZE];
    unsigned char u[TG_MTPROTO_SHA512_LENGTH];
    unsigned char t[TG_MTPROTO_SHA512_LENGTH];
    unsigned char inner[TG_MTPROTO_SHA512_LENGTH];
    tg_sha512_context ipad_ctx;
    tg_sha512_context opad_ctx;
    tg_sha512_context ctx;
    unsigned long block_index;
    unsigned long generated;
    unsigned long copy_length;
    unsigned long i;
    unsigned int j;

    if (password == 0 || output == 0 || iterations == 0UL ||
        (salt == 0 && salt_length > 0UL) ||
        salt_length > TG_PBKDF2_SALT_BLOCK_MAX - 4UL) {
        return 1;
    }

    /*
     * Precompute the HMAC inner/outer SHA-512 midstates once (absorb the
     * ipad/opad key blocks). Each PBKDF2 iteration then costs two block
     * transforms instead of four, roughly halving the CPU cost of the 100k
     * iteration loop on slow CPUs like the 68080.
     */
    memset(key_block, 0, sizeof(key_block));
    if (password_length > TG_SHA512_BLOCK_SIZE) {
        tg_mtproto_sha512(password, password_length, hashed_key);
        memcpy(key_block, hashed_key, sizeof(hashed_key));
    } else if (password_length > 0UL) {
        memcpy(key_block, password, (size_t)password_length);
    }
    for (j = 0U; j < TG_SHA512_BLOCK_SIZE; ++j) {
        pad[j] = (unsigned char)(key_block[j] ^ 0x36U);
    }
    tg_sha512_init(&ipad_ctx);
    tg_sha512_update(&ipad_ctx, pad, sizeof(pad));
    for (j = 0U; j < TG_SHA512_BLOCK_SIZE; ++j) {
        pad[j] = (unsigned char)(key_block[j] ^ 0x5cU);
    }
    tg_sha512_init(&opad_ctx);
    tg_sha512_update(&opad_ctx, pad, sizeof(pad));

    generated = 0UL;
    block_index = 1UL;
    while (generated < output_length) {
        if (salt_length > 0UL) {
            memcpy(salt_block, salt, (size_t)salt_length);
        }
        salt_block[salt_length] = (unsigned char)((block_index >> 24) & 0xffUL);
        salt_block[salt_length + 1UL] =
            (unsigned char)((block_index >> 16) & 0xffUL);
        salt_block[salt_length + 2UL] =
            (unsigned char)((block_index >> 8) & 0xffUL);
        salt_block[salt_length + 3UL] =
            (unsigned char)(block_index & 0xffUL);

        /* U_1 = HMAC(password, salt || INT(block_index)) via the midstates. */
        ctx = ipad_ctx;
        tg_sha512_update(&ctx, salt_block, salt_length + 4UL);
        tg_sha512_final(&ctx, inner);
        ctx = opad_ctx;
        tg_sha512_update(&ctx, inner, sizeof(inner));
        tg_sha512_final(&ctx, u);
        memcpy(t, u, sizeof(t));

        for (i = 1UL; i < iterations; ++i) {
            ctx = ipad_ctx;
            tg_sha512_update(&ctx, u, sizeof(u));
            tg_sha512_final(&ctx, inner);
            ctx = opad_ctx;
            tg_sha512_update(&ctx, inner, sizeof(inner));
            tg_sha512_final(&ctx, u);
            for (j = 0U; j < TG_MTPROTO_SHA512_LENGTH; ++j) {
                t[j] = (unsigned char)(t[j] ^ u[j]);
            }
            if ((i & 2047UL) == 0UL) {
                tg_mtproto_progress_tick();
            }
        }

        copy_length = output_length - generated;
        if (copy_length > TG_MTPROTO_SHA512_LENGTH) {
            copy_length = TG_MTPROTO_SHA512_LENGTH;
        }
        memcpy(output + generated, t, (size_t)copy_length);
        generated += copy_length;
        ++block_index;
    }
    return 0;
}

#if !defined(TG_NO_SELFTEST)
/* xorshift32: the same cases on every lane. */
static unsigned long tg_sha_test_next(unsigned long *state)
{
    unsigned long x;

    x = *state & 0xffffffffUL;
    x ^= (x << 13) & 0xffffffffUL;
    x ^= x >> 17;
    x ^= (x << 5) & 0xffffffffUL;
    *state = x;
    return x;
}

/* The word form of SHA-256: the FIPS two-block vector, the transform
   against the one it replaced on random states and blocks, and a digest
   taken in one call (whole blocks straight from the input) against the
   same data fed one byte at a time (every block through the context). */
static int tg_sha256_word_self_test(void)
{
    static const unsigned char two_blocks[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const unsigned char two_blocks_digest[TG_MTPROTO_SHA256_LENGTH] = {
        0x24U, 0x8dU, 0x6aU, 0x61U, 0xd2U, 0x06U, 0x38U, 0xb8U,
        0xe5U, 0xc0U, 0x26U, 0x93U, 0x0cU, 0x3eU, 0x60U, 0x39U,
        0xa3U, 0x3cU, 0xe4U, 0x59U, 0x64U, 0xffU, 0x21U, 0x67U,
        0xf6U, 0xecU, 0xedU, 0xd4U, 0x19U, 0xdbU, 0x06U, 0xc1U
    };
    static unsigned char data[4096];
    tg_sha256_context fast;
    tg_sha256_context ref;
    unsigned char one[TG_MTPROTO_SHA256_LENGTH];
    unsigned char bytewise[TG_MTPROTO_SHA256_LENGTH];
    unsigned char block[TG_SHA256_BLOCK_SIZE];
    unsigned long seed;
    unsigned long length;
    unsigned long i;
    unsigned int n;
    unsigned int j;

    tg_mtproto_sha256(two_blocks, sizeof(two_blocks) - 1U, one);
    if (memcmp(one, two_blocks_digest, sizeof(one)) != 0) {
        return 2;
    }
    seed = 0x9e3779b9UL;
    for (n = 0U; n < 64U; ++n) {
        tg_sha256_init(&fast);
        for (j = 0U; j < 8U; ++j) {
            fast.h[j] = tg_sha_test_next(&seed);
        }
        ref = fast;
        for (j = 0U; j < TG_SHA256_BLOCK_SIZE; ++j) {
            block[j] = (unsigned char)tg_sha_test_next(&seed);
        }
        tg_sha256_transform(&fast, block);
        tg_sha256_transform_ref(&ref, block);
        for (j = 0U; j < 8U; ++j) {
            if (fast.h[j] != ref.h[j]) {
                return 2;
            }
        }
    }
    for (n = 0U; n < 24U; ++n) {
        length = tg_sha_test_next(&seed) % (unsigned long)sizeof(data);
        for (i = 0UL; i < length; ++i) {
            data[i] = (unsigned char)tg_sha_test_next(&seed);
        }
        tg_mtproto_sha256(data, length, one);
        tg_sha256_init(&fast);
        for (i = 0UL; i < length; ++i) {
            tg_sha256_update(&fast, data + i, 1UL);
        }
        tg_sha256_final(&fast, bytewise);
        if (memcmp(one, bytewise, sizeof(one)) != 0) {
            return 2;
        }
    }
    return 0;
}

int tg_mtproto_crypto_self_test(void)
{
    static const unsigned char abc[] = { 'a', 'b', 'c' };
    static const unsigned char hmac_key[] = { 'k', 'e', 'y' };
    static const unsigned char hmac_data[] = {
        'T','h','e',' ','q','u','i','c','k',' ','b','r','o','w','n',' ',
        'f','o','x',' ','j','u','m','p','s',' ','o','v','e','r',' ',
        't','h','e',' ','l','a','z','y',' ','d','o','g'
    };
    static const unsigned char password[] = {
        'p','a','s','s','w','o','r','d'
    };
    static const unsigned char salt[] = { 's','a','l','t' };
    static const unsigned char sha1_abc[TG_MTPROTO_SHA1_LENGTH] = {
        0xa9U, 0x99U, 0x3eU, 0x36U, 0x47U, 0x06U, 0x81U, 0x6aU,
        0xbaU, 0x3eU, 0x25U, 0x71U, 0x78U, 0x50U, 0xc2U, 0x6cU,
        0x9cU, 0xd0U, 0xd8U, 0x9dU
    };
    static const unsigned char sha256_abc[TG_MTPROTO_SHA256_LENGTH] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
    };
    static const unsigned char sha512_abc[TG_MTPROTO_SHA512_LENGTH] = {
        0xddU, 0xafU, 0x35U, 0xa1U, 0x93U, 0x61U, 0x7aU, 0xbaU,
        0xccU, 0x41U, 0x73U, 0x49U, 0xaeU, 0x20U, 0x41U, 0x31U,
        0x12U, 0xe6U, 0xfaU, 0x4eU, 0x89U, 0xa9U, 0x7eU, 0xa2U,
        0x0aU, 0x9eU, 0xeeU, 0xe6U, 0x4bU, 0x55U, 0xd3U, 0x9aU,
        0x21U, 0x92U, 0x99U, 0x2aU, 0x27U, 0x4fU, 0xc1U, 0xa8U,
        0x36U, 0xbaU, 0x3cU, 0x23U, 0xa3U, 0xfeU, 0xebU, 0xbdU,
        0x45U, 0x4dU, 0x44U, 0x23U, 0x64U, 0x3cU, 0xe8U, 0x0eU,
        0x2aU, 0x9aU, 0xc9U, 0x4fU, 0xa5U, 0x4cU, 0xa4U, 0x9fU
    };
    static const unsigned char hmac_expected[TG_MTPROTO_SHA512_LENGTH] = {
        0xb4U, 0x2aU, 0xf0U, 0x90U, 0x57U, 0xbaU, 0xc1U, 0xe2U,
        0xd4U, 0x17U, 0x08U, 0xe4U, 0x8aU, 0x90U, 0x2eU, 0x09U,
        0xb5U, 0xffU, 0x7fU, 0x12U, 0xabU, 0x42U, 0x8aU, 0x4fU,
        0xe8U, 0x66U, 0x53U, 0xc7U, 0x3dU, 0xd2U, 0x48U, 0xfbU,
        0x82U, 0xf9U, 0x48U, 0xa5U, 0x49U, 0xf7U, 0xb7U, 0x91U,
        0xa5U, 0xb4U, 0x19U, 0x15U, 0xeeU, 0x4dU, 0x1eU, 0xc3U,
        0x93U, 0x53U, 0x57U, 0xe4U, 0xe2U, 0x31U, 0x72U, 0x50U,
        0xd0U, 0x37U, 0x2aU, 0xfaU, 0x2eU, 0xbeU, 0xebU, 0x3aU
    };
    static const unsigned char pbkdf2_expected[TG_MTPROTO_SHA512_LENGTH] = {
        0x86U, 0x7fU, 0x70U, 0xcfU, 0x1aU, 0xdeU, 0x02U, 0xcfU,
        0xf3U, 0x75U, 0x25U, 0x99U, 0xa3U, 0xa5U, 0x3dU, 0xc4U,
        0xafU, 0x34U, 0xc7U, 0xa6U, 0x69U, 0x81U, 0x5aU, 0xe5U,
        0xd5U, 0x13U, 0x55U, 0x4eU, 0x1cU, 0x8cU, 0xf2U, 0x52U,
        0xc0U, 0x2dU, 0x47U, 0x0aU, 0x28U, 0x5aU, 0x05U, 0x01U,
        0xbaU, 0xd9U, 0x99U, 0xbfU, 0xe9U, 0x43U, 0xc0U, 0x8fU,
        0x05U, 0x02U, 0x35U, 0xd7U, 0xd6U, 0x8bU, 0x1dU, 0xa5U,
        0x5eU, 0x63U, 0xf7U, 0x3bU, 0x60U, 0xa5U, 0x7fU, 0xceU
    };
    unsigned char digest256[TG_MTPROTO_SHA256_LENGTH];
    unsigned char digest512[TG_MTPROTO_SHA512_LENGTH];
    unsigned char digest1[TG_MTPROTO_SHA1_LENGTH];
    unsigned char pbkdf2[TG_MTPROTO_SHA512_LENGTH];

    tg_mtproto_sha1(abc, sizeof(abc), digest1);
    if (memcmp(digest1, sha1_abc, sizeof(sha1_abc)) != 0) {
        return 2;
    }
    tg_mtproto_sha256(abc, sizeof(abc), digest256);
    if (memcmp(digest256, sha256_abc, sizeof(sha256_abc)) != 0) {
        return 2;
    }
    tg_mtproto_sha512(abc, sizeof(abc), digest512);
    if (memcmp(digest512, sha512_abc, sizeof(sha512_abc)) != 0) {
        return 2;
    }
    tg_mtproto_hmac_sha512(hmac_key, sizeof(hmac_key), hmac_data,
                           sizeof(hmac_data), digest512);
    if (memcmp(digest512, hmac_expected, sizeof(hmac_expected)) != 0) {
        return 2;
    }
    if (tg_mtproto_pbkdf2_hmac_sha512(password, sizeof(password), salt,
                                      sizeof(salt), 1UL, pbkdf2,
                                      sizeof(pbkdf2)) != 0 ||
        memcmp(pbkdf2, pbkdf2_expected, sizeof(pbkdf2_expected)) != 0) {
        return 2;
    }
    return tg_sha256_word_self_test();
}

/* For the benchmark: the reference transform over whole blocks. */
void tg_mtproto_sha256_ref_blocks(const unsigned char *data,
                                  unsigned long length)
{
    tg_sha256_context context;

    tg_sha256_init(&context);
    while (length >= TG_SHA256_BLOCK_SIZE) {
        tg_sha256_transform_ref(&context, data);
        data += TG_SHA256_BLOCK_SIZE;
        length -= TG_SHA256_BLOCK_SIZE;
    }
}
#endif /* !TG_NO_SELFTEST */
