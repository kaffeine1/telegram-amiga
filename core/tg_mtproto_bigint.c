/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#if !defined(TG_AMIGAOS3_ENABLE_AMISSL)
#define TG_AMIGAOS3_ENABLE_AMISSL 0
#endif

#if !defined(TG_AMIGAOS4_ENABLE_AMISSL)
#define TG_AMIGAOS4_ENABLE_AMISSL 0
#endif

#if !defined(TG_ENABLE_TLS)
#define TG_ENABLE_TLS 0
#endif

/* Use a linked OpenSSL BN_mod_exp only on AROS real-OpenSSL builds
   (TG_ENABLE_TLS). Everywhere else -- AmigaOS4 and now AmigaOS3 -- use the
   in-tree Montgomery modexp below: O(n^2), no AmiSSL/OpenSSL dependency, and far
   faster than AmiSSL's BN_mod_exp on emulated/68k hardware (~60s per 2048-bit
   modexp was measured through the AmiSSL path on OS4). The 68k build links
   -nodefaultlibs, so it also defines TG_PROVIDE_MULDI3 to supply the one libgcc
   helper (__muldi3) the 64-bit Montgomery math needs. */
#if TG_ENABLE_TLS
#define TG_MTPROTO_BIGINT_USE_OPENSSL 1
#include <openssl/bn.h>
#else
#define TG_MTPROTO_BIGINT_USE_OPENSSL 0
#endif

#include "tg_mtproto_bigint.h"

#if TG_MTPROTO_BIGINT_USE_OPENSSL
static int tg_bigint_mod_exp_openssl(
    const unsigned char base[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char *exponent,
    unsigned long exponent_length,
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE],
    unsigned char output[TG_MTPROTO_BIGINT_SIZE])
{
    BN_CTX *ctx;
    BIGNUM *bn_base;
    BIGNUM *bn_exponent;
    BIGNUM *bn_modulus;
    BIGNUM *bn_result;
    int result;
    int bytes;

    if (base == 0 || exponent == 0 || modulus == 0 || output == 0) {
        return 0;
    }
    if (exponent_length > TG_MTPROTO_BIGINT_EXP_MAX) {
        exponent_length = TG_MTPROTO_BIGINT_EXP_MAX;
    }

    result = 0;
    ctx = BN_CTX_new();
    bn_base = 0;
    bn_exponent = 0;
    bn_modulus = 0;
    bn_result = 0;
    if (ctx == 0) {
        return 0;
    }

    bn_base = BN_bin2bn(base, TG_MTPROTO_BIGINT_SIZE, 0);
    bn_exponent = BN_bin2bn(exponent, (int)exponent_length, 0);
    bn_modulus = BN_bin2bn(modulus, TG_MTPROTO_BIGINT_SIZE, 0);
    bn_result = BN_new();
    if (bn_base == 0 || bn_exponent == 0 || bn_modulus == 0 ||
        bn_result == 0) {
        goto done;
    }
    if (BN_mod_exp(bn_result, bn_base, bn_exponent, bn_modulus, ctx) != 1) {
        goto done;
    }

    bytes = BN_num_bytes(bn_result);
    if (bytes < 0 || bytes > (int)TG_MTPROTO_BIGINT_SIZE) {
        goto done;
    }
    memset(output, 0, TG_MTPROTO_BIGINT_SIZE);
    if (bytes > 0) {
        BN_bn2bin(bn_result, output + TG_MTPROTO_BIGINT_SIZE -
                  (unsigned long)bytes);
    }
    result = 1;

done:
    if (bn_result != 0) {
        BN_clear_free(bn_result);
    }
    if (bn_modulus != 0) {
        BN_clear_free(bn_modulus);
    }
    if (bn_exponent != 0) {
        BN_clear_free(bn_exponent);
    }
    if (bn_base != 0) {
        BN_clear_free(bn_base);
    }
    BN_CTX_free(ctx);
    return result;
}
#endif

static int tg_bigint_bit(const unsigned char *a,
                         unsigned long a_length,
                         unsigned long bit)
{
    unsigned long byte_index;
    unsigned int bit_index;

    if (a == 0 || a_length == 0UL) {
        return 0;
    }
    byte_index = a_length - 1UL - (bit / 8UL);
    bit_index = (unsigned int)(bit % 8UL);
    return (a[byte_index] & (1U << bit_index)) != 0U;
}

static int tg_bigint_cmp_len(const unsigned char *a,
                             const unsigned char *b,
                             unsigned long length)
{
    unsigned long i;

    for (i = 0UL; i < length; ++i) {
        if (a[i] < b[i]) {
            return -1;
        }
        if (a[i] > b[i]) {
            return 1;
        }
    }
    return 0;
}

static void tg_bigint_sub_len(unsigned char *a,
                              const unsigned char *b,
                              unsigned long length)
{
    long i;
    unsigned int borrow;

    borrow = 0U;
    for (i = (long)length - 1L; i >= 0L; --i) {
        unsigned int av;
        unsigned int bv;
        av = (unsigned int)a[i];
        bv = (unsigned int)b[i] + borrow;
        if (av < bv) {
            a[i] = (unsigned char)(256U + av - bv);
            borrow = 1U;
        } else {
            a[i] = (unsigned char)(av - bv);
            borrow = 0U;
        }
    }
}

static void tg_bigint_mod_reduce(
    const unsigned char input[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE],
    unsigned char out[TG_MTPROTO_BIGINT_SIZE])
{
    unsigned char one[TG_MTPROTO_BIGINT_SIZE];
    unsigned long bit;

    memset(out, 0, TG_MTPROTO_BIGINT_SIZE);
    memset(one, 0, sizeof(one));
    one[TG_MTPROTO_BIGINT_SIZE - 1U] = 1U;
    for (bit = TG_MTPROTO_BIGINT_SIZE * 8UL; bit > 0UL; --bit) {
        tg_mtproto_bigint_add_mod(out, out, modulus);
        if (tg_bigint_bit(input, TG_MTPROTO_BIGINT_SIZE, bit - 1UL)) {
            tg_mtproto_bigint_add_mod(out, one, modulus);
        }
    }
}

int tg_mtproto_bigint_cmp(
    const unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE])
{
    return tg_bigint_cmp_len(a, b, TG_MTPROTO_BIGINT_SIZE);
}

void tg_mtproto_bigint_sub(
    unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE])
{
    tg_bigint_sub_len(a, b, TG_MTPROTO_BIGINT_SIZE);
}

void tg_mtproto_bigint_add_mod(
    unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE])
{
    unsigned char sum[TG_MTPROTO_BIGINT_SIZE + 1U];
    unsigned char mod[TG_MTPROTO_BIGINT_SIZE + 1U];
    long i;
    unsigned int carry;

    memset(sum, 0, sizeof(sum));
    memset(mod, 0, sizeof(mod));
    memcpy(mod + 1U, modulus, TG_MTPROTO_BIGINT_SIZE);
    carry = 0U;
    for (i = (long)TG_MTPROTO_BIGINT_SIZE - 1L; i >= 0L; --i) {
        unsigned int value;
        value = (unsigned int)a[i] + (unsigned int)b[i] + carry;
        sum[(unsigned long)i + 1UL] = (unsigned char)(value & 0xffU);
        carry = value >> 8;
    }
    sum[0] = (unsigned char)carry;
    if (tg_bigint_cmp_len(sum, mod, sizeof(sum)) >= 0) {
        tg_bigint_sub_len(sum, mod, sizeof(sum));
    }
    memcpy(a, sum + 1U, TG_MTPROTO_BIGINT_SIZE);
}

void tg_mtproto_bigint_sub_mod(
    unsigned char out[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE])
{
    memcpy(out, a, TG_MTPROTO_BIGINT_SIZE);
    if (tg_mtproto_bigint_cmp(out, b) >= 0) {
        tg_mtproto_bigint_sub(out, b);
    } else {
        unsigned char tmp[TG_MTPROTO_BIGINT_SIZE];
        memcpy(tmp, modulus, sizeof(tmp));
        tg_mtproto_bigint_sub(tmp, b);
        tg_mtproto_bigint_add_mod(out, tmp, modulus);
    }
}

void tg_mtproto_bigint_mod_mul(
    const unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE],
    unsigned char out[TG_MTPROTO_BIGINT_SIZE])
{
    unsigned char result[TG_MTPROTO_BIGINT_SIZE];
    unsigned char addend[TG_MTPROTO_BIGINT_SIZE];
    unsigned long bit;

    memset(result, 0, sizeof(result));
    tg_bigint_mod_reduce(a, modulus, addend);
    for (bit = 0UL; bit < TG_MTPROTO_BIGINT_SIZE * 8UL; ++bit) {
        if (tg_bigint_bit(b, TG_MTPROTO_BIGINT_SIZE, bit)) {
            tg_mtproto_bigint_add_mod(result, addend, modulus);
        }
        tg_mtproto_bigint_add_mod(addend, addend, modulus);
    }
    memcpy(out, result, TG_MTPROTO_BIGINT_SIZE);
}

/* ---- Fast modular exponentiation: little-endian Montgomery (CIOS) ----
   The historical mod_exp/mod_mul above are bit-by-bit double-and-add, i.e.
   O(n^3) over a 2048-bit operand. On a slow target (AmigaOS4 on emulated PPC)
   that is ~60s per modexp, so the two DH modexps blow past the server's auth
   handshake timeout. Montgomery multiplication is O(n^2) and is ~25x faster
   here, with no AmiSSL/OpenSSL dependency. */

#define TG_BN_WORDS (TG_MTPROTO_BIGINT_SIZE / 4U)

/* The word-based Montgomery path uses 64-bit (unsigned long long) products.
   On m68k the AmiSSL build links with -nodefaultlibs and has no libgcc, so the
   64-bit multiply helper __muldi3 is unresolved. These helpers are therefore
   compiled only for the in-tree backends (AmigaOS4, AROS without TLS); the
   AmiSSL/OpenSSL builds (AmigaOS3, TLS) use BN_mod_exp plus a 32-bit bit-by-bit
   fallback instead. */
#if !TG_MTPROTO_BIGINT_USE_OPENSSL
typedef unsigned long long tg_bn_dword;   /* 64-bit accumulator */

static int tg_w_cmp(const unsigned int *a, const unsigned int *b,
                    unsigned long nw)
{
    long i;
    for (i = (long)nw - 1L; i >= 0L; --i) {
        if (a[i] < b[i]) {
            return -1;
        }
        if (a[i] > b[i]) {
            return 1;
        }
    }
    return 0;
}

static void tg_w_sub(unsigned int *a, const unsigned int *b, unsigned long nw)
{
    unsigned long i;
    tg_bn_dword borrow;

    borrow = 0ULL;
    for (i = 0UL; i < nw; ++i) {
        tg_bn_dword d;
        d = (tg_bn_dword)a[i] - (tg_bn_dword)b[i] - borrow;
        a[i] = (unsigned int)d;
        borrow = (d >> 32) & 1ULL;
    }
}

static void tg_w_double_mod(unsigned int *x, const unsigned int *m,
                            unsigned long nw)
{
    unsigned long i;
    unsigned int carry;

    carry = 0U;
    for (i = 0UL; i < nw; ++i) {
        tg_bn_dword v;
        v = ((tg_bn_dword)x[i] << 1) | carry;
        x[i] = (unsigned int)v;
        carry = (unsigned int)(v >> 32) & 1U;
    }
    if (carry != 0U || tg_w_cmp(x, m, nw) >= 0) {
        tg_w_sub(x, m, nw);
    }
}

/* -m^-1 mod 2^32, m odd (Newton's iteration; doubles correct bits each step) */
static unsigned int tg_w_inv0(unsigned int m0)
{
    unsigned int inv;
    inv = m0;                         /* correct mod 8 */
    inv = inv * (2U - m0 * inv);      /* mod 2^6  */
    inv = inv * (2U - m0 * inv);      /* mod 2^12 */
    inv = inv * (2U - m0 * inv);      /* mod 2^24 */
    inv = inv * (2U - m0 * inv);      /* mod 2^48 -> covers 2^32 */
    return (unsigned int)(0U - inv);  /* -m0^-1 mod 2^32 */
}

#if defined(TG_MTPROTO_BIGINT_M68K_ASM)
#include <exec/execbase.h>
#include <proto/exec.h>
/* The 68060 does NOT implement the 64-bit-result mulu.l (mulu.l Dn,Dh:Dl) used
   by the asm below: it traps to the 060 software package, or faults if that is
   not installed. So the build is compiled -m68060 (GCC then emits __muldi3 for
   64-bit C multiplies instead of that instruction), the asm re-enables the
   instruction only for its own block via ".chip 68040", and at runtime the asm
   path is taken ONLY on CPUs that actually have it: 68020/030/040 and the
   68080/Vampire (which reports AFB_68060=0, seen as 68040 + extensions). A real
   68060 (AFB_68060=1) and a bare 68000/010 fall back to the portable C path. */
static int tg_mont_hw64mul(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = ((SysBase->AttnFlags & AFF_68020) &&
                  !(SysBase->AttnFlags & AFF_68060)) ? 1 : 0;
    }
    return cached;
}
#endif

/* {dst[k], carry} = src[k] + b[k]*w + carry, for k = 0..n-1; returns the final
   carry word. dst may trail src by one word (the Montgomery reduction shift).
   On a CPU with the hardware 64-bit mulu.l this is hand-written asm: at -O0 (the
   only level this toolchain compiles correctly) the C multiply-accumulate is
   dominated by stack traffic, while the asm keeps the row in registers with
   mulu.l (32x32->64) and addx. Everywhere else -- the 68060, and every non-68k
   target -- the portable C version runs (it is also the cross-check). */
static unsigned int tg_mont_mac(unsigned int *dst, const unsigned int *src,
                                const unsigned int *b, unsigned int w,
                                unsigned int carry, unsigned long n)
{
#if defined(TG_MTPROTO_BIGINT_M68K_ASM)
    if (tg_mont_hw64mul()) {
        /* ".chip 68040" re-enables the 64-bit mulu.l for this block even though
           the file is built -m68060; the runtime check above guarantees we only
           reach here on a CPU that implements it. ".chip 68060" restores the
           assembler so later GCC code stays 68060-safe. */
        __asm__ volatile (
            "    .chip 68040\n"
            "    tst.l   %5\n"
            "    beq.s   2f\n"
            "    moveq   #0,%%d4\n"
            "1:\n"
            "    move.l  (%2)+,%%d0\n"        /* d0 = b[k]               */
            "    mulu.l  %3,%%d1:%%d0\n"      /* d1:d0 = b[k] * w        */
            "    add.l   %4,%%d0\n"           /* += carry (low)          */
            "    addx.l  %%d4,%%d1\n"         /*   carry into high       */
            "    add.l   (%1)+,%%d0\n"        /* += src[k] (low)         */
            "    addx.l  %%d4,%%d1\n"         /*   carry into high       */
            "    move.l  %%d0,(%0)+\n"        /* dst[k] = low 32         */
            "    move.l  %%d1,%4\n"           /* carry  = high 32        */
            "    subq.l  #1,%5\n"
            "    bne.s   1b\n"
            "2:\n"
            "    .chip 68060\n"
            : "+a"(dst), "+a"(src), "+a"(b), "+d"(w), "+d"(carry), "+d"(n)
            :
            : "d0", "d1", "d4", "cc", "memory");
        return carry;
    }
#endif
    {
        unsigned long k;
        for (k = 0UL; k < n; ++k) {
            tg_bn_dword s;
            s = (tg_bn_dword)src[k] + (tg_bn_dword)b[k] * (tg_bn_dword)w +
                (tg_bn_dword)carry;
            dst[k] = (unsigned int)s;
            carry = (unsigned int)(s >> 32);
        }
        return carry;
    }
}

/* out = a*b*R^-1 mod m (CIOS), little-endian 32-bit words, m odd,
   n_inv0 = -m^-1 mod 2^32 */
static void tg_mont_mul_w(unsigned int *out, const unsigned int *a,
                          const unsigned int *b, const unsigned int *m,
                          unsigned int n_inv0, unsigned long nw)
{
    unsigned int t[TG_BN_WORDS + 2U];
    unsigned long i;
    tg_bn_dword sum;
    unsigned int mm;
    unsigned int carry;

    for (i = 0UL; i < nw + 2UL; ++i) {
        t[i] = 0U;
    }
    for (i = 0UL; i < nw; ++i) {
        carry = tg_mont_mac(t, t, a, b[i], 0U, nw);   /* t += a * b[i] */
        sum = (tg_bn_dword)t[nw] + (tg_bn_dword)carry;
        t[nw] = (unsigned int)sum;
        t[nw + 1UL] = (unsigned int)(sum >> 32);

        mm = (unsigned int)((tg_bn_dword)t[0] * (tg_bn_dword)n_inv0);
        carry = (unsigned int)(((tg_bn_dword)t[0] +
                 (tg_bn_dword)mm * (tg_bn_dword)m[0]) >> 32);
        carry = tg_mont_mac(t, t + 1UL, m + 1UL, mm, carry, nw - 1UL); /* reduce */

        sum = (tg_bn_dword)t[nw] + (tg_bn_dword)carry;
        t[nw - 1UL] = (unsigned int)sum;
        t[nw] = t[nw + 1UL] + (unsigned int)(sum >> 32);
    }
    for (i = 0UL; i < nw; ++i) {
        out[i] = t[i];
    }
    if (t[nw] != 0U || tg_w_cmp(out, m, nw) >= 0) {
        tg_w_sub(out, m, nw);
    }
}
static void tg_mtproto_secure_zero(void *data, unsigned long length)
{
    volatile unsigned char *bytes;

    bytes = (volatile unsigned char *)data;
    while (length > 0UL) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}
#endif /* !TG_MTPROTO_BIGINT_USE_OPENSSL */

void tg_mtproto_bigint_mod_exp(
    const unsigned char base[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char *exponent,
    unsigned long exponent_length,
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE],
    unsigned char output[TG_MTPROTO_BIGINT_SIZE])
{
#if TG_MTPROTO_BIGINT_USE_OPENSSL
    unsigned char result[TG_MTPROTO_BIGINT_SIZE];
    unsigned char power[TG_MTPROTO_BIGINT_SIZE];
    long bit;

    if (tg_bigint_mod_exp_openssl(base, exponent, exponent_length, modulus,
                                  output)) {
        return;
    }
    /* OpenSSL/AmiSSL BN unavailable or failed (rare): bit-by-bit
       double-and-add fallback. It uses only 32-bit operations, so the m68k
       AmiSSL build (which links with -nodefaultlibs and has no libgcc) needs no
       64-bit multiply helper. The word-based Montgomery below is compiled only
       for the in-tree backends (AmigaOS4, AROS without TLS). */
    if (exponent_length > TG_MTPROTO_BIGINT_EXP_MAX) {
        exponent_length = TG_MTPROTO_BIGINT_EXP_MAX;
    }
    memset(result, 0, sizeof(result));
    result[TG_MTPROTO_BIGINT_SIZE - 1U] = 1U;
    tg_bigint_mod_reduce(base, modulus, power);
    for (bit = 0L; bit < (long)(exponent_length * 8UL); ++bit) {
        if (tg_bigint_bit(exponent, exponent_length, (unsigned long)bit)) {
            tg_mtproto_bigint_mod_mul(result, power, modulus, result);
        }
        tg_mtproto_bigint_mod_mul(power, power, modulus, power);
    }
    memcpy(output, result, TG_MTPROTO_BIGINT_SIZE);
#else
    unsigned long nb = TG_MTPROTO_BIGINT_SIZE;
    unsigned long nw = TG_BN_WORDS;
    unsigned int m_w[TG_BN_WORDS];
    unsigned int base_w[TG_BN_WORDS];
    unsigned int rr[TG_BN_WORDS];
    unsigned int mont_base[TG_BN_WORDS];
    unsigned int x[TG_BN_WORDS];
    unsigned int one_w[TG_BN_WORDS];
    unsigned int tmp[TG_BN_WORDS];
    unsigned int n_inv0;
    unsigned long i;
    unsigned long k;
    long top;
    long bit;

    if (exponent_length > TG_MTPROTO_BIGINT_EXP_MAX) {
        exponent_length = TG_MTPROTO_BIGINT_EXP_MAX;
    }

    /* Montgomery needs an odd modulus; fall back to the simple bit method for
       an even modulus (does not occur for DH/RSA). */
    if ((modulus[nb - 1UL] & 1U) == 0U) {
        unsigned char result[TG_MTPROTO_BIGINT_SIZE];
        unsigned char power[TG_MTPROTO_BIGINT_SIZE];
        memset(result, 0, sizeof(result));
        result[TG_MTPROTO_BIGINT_SIZE - 1U] = 1U;
        tg_bigint_mod_reduce(base, modulus, power);
        for (bit = 0L; bit < (long)(exponent_length * 8UL); ++bit) {
            if (tg_bigint_bit(exponent, exponent_length, (unsigned long)bit)) {
                tg_mtproto_bigint_mod_mul(result, power, modulus, result);
            }
            tg_mtproto_bigint_mod_mul(power, power, modulus, power);
        }
        memcpy(output, result, TG_MTPROTO_BIGINT_SIZE);
        return;
    }

    /* big-endian bytes -> little-endian 32-bit words */
    for (k = 0UL; k < nw; ++k) {
        unsigned long p = nb - 4UL * (k + 1UL);
        m_w[k] = ((unsigned int)modulus[p] << 24) |
                 ((unsigned int)modulus[p + 1UL] << 16) |
                 ((unsigned int)modulus[p + 2UL] << 8) |
                 (unsigned int)modulus[p + 3UL];
        base_w[k] = ((unsigned int)base[p] << 24) |
                    ((unsigned int)base[p + 1UL] << 16) |
                    ((unsigned int)base[p + 2UL] << 8) |
                    (unsigned int)base[p + 3UL];
    }

    n_inv0 = tg_w_inv0(m_w[0]);

    /* reduce base modulo modulus (DH/RSA base < modulus, so 0-1 subtraction) */
    while (tg_w_cmp(base_w, m_w, nw) >= 0) {
        tg_w_sub(base_w, m_w, nw);
    }

    /* rr = R^2 mod m, R = 2^(32*nw): start at 1, double 2*(32*nw) times mod m */
    for (i = 0UL; i < nw; ++i) {
        rr[i] = 0U;
    }
    rr[0] = 1U;
    for (k = 0UL; k < 64UL * nw; ++k) {
        tg_w_double_mod(rr, m_w, nw);
    }

    for (i = 0UL; i < nw; ++i) {
        one_w[i] = 0U;
    }
    one_w[0] = 1U;
    tg_mont_mul_w(x, one_w, rr, m_w, n_inv0, nw);          /* x = mont(1) */
    tg_mont_mul_w(mont_base, base_w, rr, m_w, n_inv0, nw); /* base * R mod m */

    top = -1L;
    for (bit = (long)(exponent_length * 8UL) - 1L; bit >= 0L; --bit) {
        if (tg_bigint_bit(exponent, exponent_length, (unsigned long)bit)) {
            top = bit;
            break;
        }
    }

    if (top < 64L) {
        /* Small exponent (e.g. RSA e=65537): 1-bit left-to-right avoids table setup. */
        for (bit = top; bit >= 0L; --bit) {
            tg_mont_mul_w(tmp, x, x, m_w, n_inv0, nw);
            memcpy(x, tmp, sizeof(tmp));
            if (tg_bigint_bit(exponent, exponent_length, (unsigned long)bit)) {
                tg_mont_mul_w(tmp, x, mont_base, m_w, n_inv0, nw);
                memcpy(x, tmp, sizeof(tmp));
            }
        }
    } else {
        /* 4-bit fixed window in Montgomery form: cuts ~1024 multiplies down to
           14 precomputations + <=512 window multiplies for 2048-bit DH/SRP. */
        static unsigned int mont_pow[16][TG_BN_WORDS];
        unsigned int w;

        memcpy(mont_pow[1], mont_base, sizeof(mont_base));
        for (w = 2U; w < 16U; ++w) {
            tg_mont_mul_w(mont_pow[w], mont_pow[w - 1U], mont_base,
                          m_w, n_inv0, nw);
        }
        bit = top;
        while (bit >= 3L) {
            unsigned int win =
                (tg_bigint_bit(exponent, exponent_length,
                               (unsigned long)bit) ? 8U : 0U) |
                (tg_bigint_bit(exponent, exponent_length,
                               (unsigned long)(bit - 1L)) ? 4U : 0U) |
                (tg_bigint_bit(exponent, exponent_length,
                               (unsigned long)(bit - 2L)) ? 2U : 0U) |
                (tg_bigint_bit(exponent, exponent_length,
                               (unsigned long)(bit - 3L)) ? 1U : 0U);
            tg_mont_mul_w(tmp, x, x, m_w, n_inv0, nw);
            tg_mont_mul_w(x, tmp, tmp, m_w, n_inv0, nw);
            tg_mont_mul_w(tmp, x, x, m_w, n_inv0, nw);
            tg_mont_mul_w(x, tmp, tmp, m_w, n_inv0, nw);
            if (win != 0U) {
                tg_mont_mul_w(tmp, x, mont_pow[win], m_w, n_inv0, nw);
                memcpy(x, tmp, sizeof(tmp));
            }
            bit -= 4L;
        }
        while (bit >= 0L) {
            tg_mont_mul_w(tmp, x, x, m_w, n_inv0, nw);
            memcpy(x, tmp, sizeof(tmp));
            if (tg_bigint_bit(exponent, exponent_length, (unsigned long)bit)) {
                tg_mont_mul_w(tmp, x, mont_pow[1], m_w, n_inv0, nw);
                memcpy(x, tmp, sizeof(tmp));
            }
            --bit;
        }
        tg_mtproto_secure_zero(mont_pow, sizeof(mont_pow));
    }

    /* leave Montgomery form: result = x * R^-1 mod m = mont_mul(x, 1) */
    tg_mont_mul_w(tmp, x, one_w, m_w, n_inv0, nw);
    for (k = 0UL; k < nw; ++k) {
        unsigned long p = nb - 4UL * (k + 1UL);
        output[p] = (unsigned char)(tmp[k] >> 24);
        output[p + 1UL] = (unsigned char)(tmp[k] >> 16);
        output[p + 2UL] = (unsigned char)(tmp[k] >> 8);
        output[p + 3UL] = (unsigned char)tmp[k];
    }
    tg_mtproto_secure_zero(base_w, sizeof(base_w));
    tg_mtproto_secure_zero(mont_base, sizeof(mont_base));
    tg_mtproto_secure_zero(x, sizeof(x));
    tg_mtproto_secure_zero(tmp, sizeof(tmp));
#endif /* TG_MTPROTO_BIGINT_USE_OPENSSL */
}

void tg_mtproto_bigint_from_u32(
    unsigned long value,
    unsigned char out[TG_MTPROTO_BIGINT_SIZE])
{
    memset(out, 0, TG_MTPROTO_BIGINT_SIZE);
    out[TG_MTPROTO_BIGINT_SIZE - 4U] = (unsigned char)((value >> 24) & 0xffU);
    out[TG_MTPROTO_BIGINT_SIZE - 3U] = (unsigned char)((value >> 16) & 0xffU);
    out[TG_MTPROTO_BIGINT_SIZE - 2U] = (unsigned char)((value >> 8) & 0xffU);
    out[TG_MTPROTO_BIGINT_SIZE - 1U] = (unsigned char)(value & 0xffU);
}

void tg_mtproto_bigint_mul_bytes(
    const unsigned char *a,
    unsigned long a_length,
    const unsigned char *b,
    unsigned long b_length,
    unsigned char *out,
    unsigned long out_length)
{
    unsigned long ia;

    if (out == 0 || out_length == 0UL) {
        return;
    }
    memset(out, 0, (size_t)out_length);
    if (a == 0 || b == 0 || a_length == 0UL || b_length == 0UL) {
        return;
    }
    for (ia = 0UL; ia < a_length && ia < out_length; ++ia) {
        unsigned long ib;
        unsigned long carry;
        unsigned int av;
        av = (unsigned int)a[a_length - 1UL - ia];
        carry = 0UL;
        for (ib = 0UL; ib < b_length && ia + ib < out_length; ++ib) {
            unsigned long out_pos;
            unsigned long value;
            out_pos = out_length - 1UL - (ia + ib);
            value = (unsigned long)out[out_pos] +
                    ((unsigned long)av *
                     (unsigned long)b[b_length - 1UL - ib]) +
                    carry;
            out[out_pos] = (unsigned char)(value & 0xffUL);
            carry = value >> 8;
        }
        for (ib = ia + b_length; carry != 0UL && ib < out_length; ++ib) {
            unsigned long out_pos;
            unsigned long value;
            out_pos = out_length - 1UL - ib;
            value = (unsigned long)out[out_pos] + carry;
            out[out_pos] = (unsigned char)(value & 0xffUL);
            carry = value >> 8;
        }
    }
}

void tg_mtproto_bigint_add_bytes(
    unsigned char *a,
    unsigned long a_length,
    const unsigned char *b,
    unsigned long b_length)
{
    unsigned long i;
    unsigned int carry;

    if (a == 0 || b == 0 || a_length == 0UL || b_length == 0UL) {
        return;
    }
    carry = 0U;
    for (i = 0UL; i < a_length; ++i) {
        unsigned long ai;
        unsigned int bv;
        unsigned int value;
        ai = a_length - 1UL - i;
        bv = 0U;
        if (i < b_length) {
            bv = (unsigned int)b[b_length - 1UL - i];
        }
        value = (unsigned int)a[ai] + bv + carry;
        a[ai] = (unsigned char)(value & 0xffU);
        carry = value >> 8;
    }
}

unsigned long tg_mtproto_bigint_trim(
    const unsigned char *data,
    unsigned long data_length)
{
    unsigned long offset;

    offset = 0UL;
    while (offset + 1UL < data_length && data[offset] == 0U) {
        ++offset;
    }
    return offset;
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_bigint_self_test(void)
{
    unsigned char a[TG_MTPROTO_BIGINT_SIZE];
    unsigned char b[TG_MTPROTO_BIGINT_SIZE];
    unsigned char m[TG_MTPROTO_BIGINT_SIZE];
    unsigned char out[TG_MTPROTO_BIGINT_SIZE];
    unsigned char exp[1];
    unsigned char mul_out[8];
    unsigned char mul_a[2];
    unsigned char mul_b[1];

    tg_mtproto_bigint_from_u32(5UL, a);
    tg_mtproto_bigint_from_u32(9UL, b);
    tg_mtproto_bigint_from_u32(13UL, m);
    tg_mtproto_bigint_add_mod(a, b, m);
    if (a[TG_MTPROTO_BIGINT_SIZE - 1U] != 1U) {
        return 1;
    }

    tg_mtproto_bigint_from_u32(3UL, a);
    tg_mtproto_bigint_from_u32(4UL, b);
    tg_mtproto_bigint_mod_mul(a, b, m, out);
    if (out[TG_MTPROTO_BIGINT_SIZE - 1U] != 12U) {
        return 1;
    }

    tg_mtproto_bigint_from_u32(2UL, a);
    tg_mtproto_bigint_from_u32(17UL, m);
    exp[0] = 10U;
    tg_mtproto_bigint_mod_exp(a, exp, sizeof(exp), m, out);
    if (out[TG_MTPROTO_BIGINT_SIZE - 1U] != 4U) {
        return 1;
    }
    /* Exercise the >=64-bit 4-bit windowed Montgomery path + tail bits:
       mod 13 has totient 12, where 2^even == 4 (mod 12) and 2^odd == 8 (mod 12).
       exp = 2^74 + 2^33 + 5 == (4 + 8 + 5) mod 12 == 5 (mod 12),
       so 2^exp mod 13 == 2^5 mod 13 == 6. */
    {
        unsigned char exp_large[10];
        tg_mtproto_bigint_from_u32(13UL, m);
        memset(exp_large, 0, sizeof(exp_large));
        exp_large[0] = 0x04U; /* bit 74 (even): 2^74 mod 12 == 4 */
        exp_large[5] = 0x02U; /* bit 33 (odd):  2^33 mod 12 == 8 */
        exp_large[9] = 0x05U; /* + 5 -> total exponent mod 12 == 5 */
        tg_mtproto_bigint_mod_exp(a, exp_large, sizeof(exp_large), m, out);
        if (out[TG_MTPROTO_BIGINT_SIZE - 1U] != 6U) {
            return 1;
        }
    }

    tg_mtproto_bigint_from_u32(3UL, a);
    tg_mtproto_bigint_from_u32(5UL, b);
    tg_mtproto_bigint_from_u32(13UL, m);
    tg_mtproto_bigint_sub_mod(out, a, b, m);
    if (out[TG_MTPROTO_BIGINT_SIZE - 1U] != 11U) {
        return 1;
    }

    mul_a[0] = 0x12U;
    mul_a[1] = 0x34U;
    mul_b[0] = 0x10U;
    tg_mtproto_bigint_mul_bytes(mul_a, sizeof(mul_a), mul_b, sizeof(mul_b),
                                mul_out, sizeof(mul_out));
    if (mul_out[5] != 0x01U || mul_out[6] != 0x23U ||
        mul_out[7] != 0x40U) {
        return 1;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */

#if defined(TG_PROVIDE_MULDI3) && TG_PROVIDE_MULDI3
/*
 * 64-bit multiply for the AmigaOS 3 m68k build, which links -nodefaultlibs (no
 * libgcc). The in-tree Montgomery modexp above uses 64-bit multiplies that GCC
 * lowers to __muldi3; supply it here. Built from 16x16->32 partial products so
 * it never recurses into a 64-bit multiply. Pure C, endian-neutral; correct on
 * a 32-bit-long target (validated against the native product over 30M cases).
 */
unsigned long long __muldi3(unsigned long long a, unsigned long long b);
unsigned long long __muldi3(unsigned long long a, unsigned long long b)
{
    unsigned long a_lo;
    unsigned long a_hi;
    unsigned long b_lo;
    unsigned long b_hi;
    unsigned long al;
    unsigned long ah;
    unsigned long bl;
    unsigned long bh;
    unsigned long ll;
    unsigned long lh;
    unsigned long hl;
    unsigned long hh;
    unsigned long cross;
    unsigned long res_lo;
    unsigned long res_hi;
    unsigned long long result;

    a_lo = (unsigned long)a;
    a_hi = (unsigned long)(a >> 32);
    b_lo = (unsigned long)b;
    b_hi = (unsigned long)(b >> 32);

    al = a_lo & 0xFFFFUL;
    ah = a_lo >> 16;
    bl = b_lo & 0xFFFFUL;
    bh = b_lo >> 16;

    ll = al * bl;
    lh = al * bh;
    hl = ah * bl;
    hh = ah * bh;

    cross = (ll >> 16) + (lh & 0xFFFFUL) + (hl & 0xFFFFUL);
    res_lo = (ll & 0xFFFFUL) | ((cross & 0xFFFFUL) << 16);
    res_hi = hh + (lh >> 16) + (hl >> 16) + (cross >> 16);

    result = ((unsigned long long)res_hi << 32) | (unsigned long long)res_lo;
    result += ((unsigned long long)(a_lo * b_hi + a_hi * b_lo)) << 32;
    return result;
}
#endif /* TG_PROVIDE_MULDI3 */
