/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "tg_mtproto_auth.h"
#include "tg_mtproto_rsa.h"

#define TG_MTPROTO_VECTOR_CONSTRUCTOR 0x1cb5c415UL
#define TG_MTPROTO_RES_PQ_CONSTRUCTOR 0x05162463UL

#if defined(__amigaos3__) && defined(__m68k__)
unsigned long long __udivdi3(unsigned long long num, unsigned long long den)
{
    unsigned long long bit;
    unsigned long long result;

    if (den == 0ULL) {
        return 0ULL;
    }

    bit = 1ULL;
    result = 0ULL;
    while (den < num && (den & 0x8000000000000000ULL) == 0ULL) {
        den <<= 1;
        bit <<= 1;
    }

    while (bit != 0ULL) {
        if (num >= den) {
            num -= den;
            result |= bit;
        }
        den >>= 1;
        bit >>= 1;
    }

    return result;
}

unsigned long long __umoddi3(unsigned long long num, unsigned long long den)
{
    unsigned long long bit;

    if (den == 0ULL) {
        return 0ULL;
    }

    bit = 1ULL;
    while (den < num && (den & 0x8000000000000000ULL) == 0ULL) {
        den <<= 1;
        bit <<= 1;
    }

    while (bit != 0ULL) {
        if (num >= den) {
            num -= den;
        }
        den >>= 1;
        bit >>= 1;
    }

    return num;
}
#endif

static unsigned long long tg_mtproto_u64_from_be(const unsigned char *data,
                                                 unsigned long length)
{
    unsigned long long value;
    unsigned long i;

    value = 0ULL;
    for (i = 0; i < length; ++i) {
        value = (value << 8) | (unsigned long long)data[i];
    }
    return value;
}

#if !defined(TG_NO_SELFTEST)
/* The pq split as it ran before 0.0.95, kept as the reference for the
   self-test and the bench: the key exchange now runs tg_mtproto_pq_rho(),
   the same steps in Montgomery form, in a file the 68k builds with -O2. */
static unsigned long long tg_mtproto_u64_gcd(unsigned long long a,
                                             unsigned long long b)
{
    unsigned long long t;

    while (b != 0ULL) {
        t = a % b;
        a = b;
        b = t;
    }

    return a;
}

static unsigned long long tg_mtproto_u64_mod_add(unsigned long long a,
                                                 unsigned long long b,
                                                 unsigned long long modulus)
{
    if (a >= modulus - b) {
        return a - (modulus - b);
    }
    return a + b;
}

static unsigned long long tg_mtproto_u64_mod_mul(unsigned long long a,
                                                 unsigned long long b,
                                                 unsigned long long modulus)
{
    unsigned long long result;

    result = 0ULL;
    a %= modulus;
    while (b > 0ULL) {
        if ((b & 1ULL) != 0ULL) {
            result = tg_mtproto_u64_mod_add(result, a, modulus);
        }
        b >>= 1;
        if (b > 0ULL) {
            a = tg_mtproto_u64_mod_add(a, a, modulus);
        }
    }

    return result;
}

/* Brent's variant of Pollard's rho. Compared with the Floyd version it uses
   fewer modular multiplications per step and, crucially, batches the gcd so a
   64-bit modular gcd (a slow software division on 32-bit targets such as
   AmigaOS4) runs once per ~128 steps instead of every step. This is what made
   pq factoring fast enough on emulated PPC (was ~120s, then a few seconds). */
unsigned long long tg_mtproto_pq_rho_ref(unsigned long long n,
                                         unsigned long long c)
{
    unsigned long long x;
    unsigned long long y;
    unsigned long long ys;
    unsigned long long q;
    unsigned long long g;
    unsigned long long r;
    unsigned long long diff;
    unsigned long long i;
    unsigned long long k;
    unsigned long long limit;
    const unsigned long long m = 128ULL;

    if ((n & 1ULL) == 0ULL) {
        return 2ULL;
    }

    y = 2ULL;
    x = 2ULL;
    ys = 2ULL;
    q = 1ULL;
    g = 1ULL;
    r = 1ULL;
    while (g == 1ULL) {
        x = y;
        for (i = 0ULL; i < r; ++i) {
            y = (tg_mtproto_u64_mod_mul(y, y, n) + c) % n;
        }
        k = 0ULL;
        while (k < r && g == 1ULL) {
            ys = y;
            limit = (r - k) < m ? (r - k) : m;
            for (i = 0ULL; i < limit; ++i) {
                y = (tg_mtproto_u64_mod_mul(y, y, n) + c) % n;
                diff = x > y ? x - y : y - x;
                if (diff != 0ULL) {
                    q = tg_mtproto_u64_mod_mul(q, diff, n);
                }
            }
            g = tg_mtproto_u64_gcd(q, n);
            k += m;
        }
        r <<= 1;
        if (r > 4000000ULL) {
            break;
        }
    }

    if (g == n || g <= 1ULL) {
        g = 1ULL;
        do {
            ys = (tg_mtproto_u64_mod_mul(ys, ys, n) + c) % n;
            diff = x > ys ? x - ys : ys - x;
            g = tg_mtproto_u64_gcd(diff, n);
        } while (g == 1ULL);
    }

    if (g > 1ULL && g < n) {
        return g;
    }
    return 0ULL;
}
#endif /* !TG_NO_SELFTEST */

static int tg_mtproto_fingerprint_equal(const tg_mtproto_fingerprint *a,
                                        const tg_mtproto_fingerprint *b)
{
    return a != 0 && b != 0 && a->hi == b->hi && a->lo == b->lo;
}

tg_mtproto_tl_status tg_mtproto_parse_res_pq(
    const unsigned char *payload,
    unsigned long payload_length,
    tg_mtproto_res_pq *out)
{
    tg_mtproto_tl_reader reader;
    const unsigned char *raw;
    const unsigned char *pq;
    unsigned long constructor;
    unsigned long message_length;
    unsigned long pq_length;
    unsigned long vector_constructor;
    unsigned long count;
    unsigned long i;

    if (payload == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    tg_mtproto_tl_reader_init(&reader, payload, payload_length);

    if (tg_mtproto_tl_read_u64(&reader, &constructor, &message_length) !=
            TG_MTPROTO_TL_OK ||
        constructor != 0UL || message_length != 0UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    if (tg_mtproto_tl_read_u64(&reader, &constructor, &message_length) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }

    if (tg_mtproto_tl_read_u32(&reader, &message_length) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    if (message_length > payload_length || payload_length - 20UL < message_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != TG_MTPROTO_RES_PQ_CONSTRUCTOR) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->nonce, raw, 16U);

    if (tg_mtproto_tl_read_raw(&reader, &raw, 16UL) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    memcpy(out->server_nonce, raw, 16U);

    if (tg_mtproto_tl_read_bytes(&reader, &pq, &pq_length) !=
        TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    if (pq_length == 0 || pq_length > sizeof(out->pq)) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->pq, pq, (size_t)pq_length);
    out->pq_length = pq_length;

    if (tg_mtproto_tl_read_u32(&reader, &vector_constructor) !=
            TG_MTPROTO_TL_OK ||
        vector_constructor != TG_MTPROTO_VECTOR_CONSTRUCTOR ||
        tg_mtproto_tl_read_u32(&reader, &count) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    if (count > TG_MTPROTO_AUTH_MAX_FINGERPRINTS) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    out->fingerprint_count = (unsigned int)count;
    for (i = 0; i < count; ++i) {
        if (tg_mtproto_tl_read_u64(&reader, &out->fingerprints[i].hi,
                                   &out->fingerprints[i].lo) !=
            TG_MTPROTO_TL_OK) {
            return TG_MTPROTO_TL_TRUNCATED;
        }
    }

    if (reader.offset != 20UL + message_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    return TG_MTPROTO_TL_OK;
}

int tg_mtproto_res_pq_nonce_matches(const tg_mtproto_res_pq *res_pq,
                                    const unsigned char nonce[16])
{
    return res_pq != 0 && nonce != 0 &&
           memcmp(res_pq->nonce, nonce, 16U) == 0;
}

int tg_mtproto_pq_factor(const unsigned char *pq,
                         unsigned long pq_length,
                         unsigned long *p,
                         unsigned long *q)
{
    unsigned long long n;
    unsigned long long factor;
    unsigned long c;
    unsigned long small;
    unsigned long large;

    if (pq == 0 || pq_length == 0 || pq_length > 8UL || p == 0 || q == 0) {
        return 1;
    }

    n = tg_mtproto_u64_from_be(pq, pq_length);
    if (n < 4ULL) {
        return 1;
    }

    if ((n & 1ULL) == 0ULL) {
        small = 2UL;
        large = (unsigned long)(n / 2ULL);
        *p = small;
        *q = large;
        return 0;
    }

    for (c = 1UL; c < 32UL; ++c) {
        factor = tg_mtproto_pq_rho(n, (unsigned long long)c);
        if (factor > 1ULL && factor < n &&
            factor <= 0xffffffffULL &&
            (n / factor) <= 0xffffffffULL) {
            small = (unsigned long)factor;
            large = (unsigned long)(n / factor);
            if (small > large) {
                *p = large;
                *q = small;
            } else {
                *p = small;
                *q = large;
            }
            return 0;
        }
    }

    return 1;
}

const tg_mtproto_fingerprint *tg_mtproto_select_fingerprint(
    const tg_mtproto_res_pq *res_pq,
    const tg_mtproto_fingerprint *known,
    unsigned int known_count)
{
    unsigned int i;
    unsigned int j;

    if (res_pq == 0 || known == 0) {
        return 0;
    }

    for (i = 0; i < res_pq->fingerprint_count; ++i) {
        for (j = 0; j < known_count; ++j) {
            if (tg_mtproto_fingerprint_equal(&res_pq->fingerprints[i],
                                             &known[j])) {
                return &res_pq->fingerprints[i];
            }
        }
    }

    return 0;
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_auth_self_test(void)
{
    static const unsigned char expected_nonce[16] = {
        0x79U, 0xf0U, 0xafU, 0xb5U, 0x02U, 0x52U, 0xe5U, 0xfcU,
        0x96U, 0x92U, 0x4bU, 0xfcU, 0xecU, 0xdaU, 0x4fU, 0x05U
    };
    static const unsigned char sample_res_pq[] = {
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x28U, 0xfbU, 0xd2U, 0xebU, 0xe5U, 0x77U, 0x67U,
        0x50U, 0x00U, 0x00U, 0x00U,
        0x63U, 0x24U, 0x16U, 0x05U,
        0x79U, 0xf0U, 0xafU, 0xb5U, 0x02U, 0x52U, 0xe5U, 0xfcU,
        0x96U, 0x92U, 0x4bU, 0xfcU, 0xecU, 0xdaU, 0x4fU, 0x05U,
        0x80U, 0x17U, 0x75U, 0xa3U, 0xefU, 0xbfU, 0xd2U, 0x70U,
        0x1aU, 0xa2U, 0x8aU, 0xd7U, 0x27U, 0xbeU, 0x46U, 0x46U,
        0x08U, 0x13U, 0x0bU, 0x74U, 0x75U, 0x66U, 0x9fU, 0xebU,
        0x8bU, 0x00U, 0x00U, 0x00U,
        0x15U, 0xc4U, 0xb5U, 0x1cU,
        0x03U, 0x00U, 0x00U, 0x00U,
        0x85U, 0xfdU, 0x64U, 0xdeU, 0x85U, 0x1dU, 0x9dU, 0xd0U,
        0xa5U, 0xb7U, 0xf7U, 0x09U, 0x35U, 0x5fU, 0xc3U, 0x0bU,
        0x21U, 0x6bU, 0xe8U, 0x6cU, 0x02U, 0x2bU, 0xb4U, 0xc3U
    };
    static const tg_mtproto_fingerprint known[] = {
        {0xd09d1d85UL, 0xde64fd85UL}
    };
    tg_mtproto_res_pq res_pq;
    const tg_mtproto_fingerprint *selected;
    unsigned long p;
    unsigned long q;

    if (tg_mtproto_parse_res_pq(sample_res_pq, sizeof(sample_res_pq),
                                &res_pq) != TG_MTPROTO_TL_OK ||
        !tg_mtproto_res_pq_nonce_matches(&res_pq, expected_nonce) ||
        res_pq.pq_length != 8UL ||
        res_pq.fingerprint_count != 3U ||
        res_pq.fingerprints[0].hi != 0xd09d1d85UL ||
        res_pq.fingerprints[0].lo != 0xde64fd85UL) {
        return 2;
    }

    if (tg_mtproto_pq_factor(res_pq.pq, res_pq.pq_length, &p, &q) != 0 ||
        p != 1141464581UL ||
        q != 1202243663UL) {
        return 2;
    }

    selected = tg_mtproto_select_fingerprint(&res_pq, known,
                                             sizeof(known) / sizeof(known[0]));
    if (selected == 0 ||
        selected->hi != known[0].hi ||
        selected->lo != known[0].lo) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
