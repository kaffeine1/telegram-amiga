/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* A QR Code encoder, written from ISO/IEC 18004 for the login screen: the
   phone of a signed-in account scans the link Telegram hands out
   (auth.exportLoginToken) and the account signs in here without a code.
   Byte mode only, error correction level M (15% of the code can be lost),
   versions 1 to 10. The mask is the one with the lowest penalty, as the
   standard asks. Checked on the Mac against the system's own QR reader
   (CoreImage) on every version and every mask; the self-test pins a few
   codes so that no lane drifts from what was checked. */

#include <stdio.h>
#include <string.h>

#include "tg_qr.h"

/* Level M, by version: error correction codewords in each block, and the
   number of blocks (ISO/IEC 18004, table 9). */
static const unsigned char tg_qr_ecc_per_block[TG_QR_MAX_VERSION + 1] = {
    0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26
};
static const unsigned char tg_qr_blocks[TG_QR_MAX_VERSION + 1] = {
    0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5
};
/* Centres of the alignment patterns, the same list on both axes (annex E). */
static const unsigned char tg_qr_align[TG_QR_MAX_VERSION + 1][3] = {
    { 0, 0, 0 }, { 0, 0, 0 }, { 6, 18, 0 }, { 6, 22, 0 }, { 6, 26, 0 },
    { 6, 30, 0 }, { 6, 34, 0 }, { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 },
    { 6, 28, 50 }
};

/* Which modules belong to the fixed patterns, and the codewords: kept off
   the stack, which is small on the 68k. */
static unsigned char tg_qr_fn[TG_QR_MAX_SIZE * TG_QR_MAX_SIZE];
static unsigned char tg_qr_data[400];
static unsigned char tg_qr_final[400];
static unsigned char tg_qr_ecc[5][32];

static int tg_qr_align_count(int version)
{
    return version == 1 ? 0 : (version < 7 ? 2 : 3);
}

/* Modules left for data and error correction once the fixed patterns are
   placed. */
static int tg_qr_raw_modules(int version)
{
    int result = (16 * version + 128) * version + 64;

    if (version >= 2) {
        int align = version / 7 + 2;

        result -= (25 * align - 10) * align - 55;
        if (version >= 7) {
            result -= 36;
        }
    }
    return result;
}

static int tg_qr_total_codewords(int version)
{
    return tg_qr_raw_modules(version) / 8;
}

static int tg_qr_data_codewords(int version)
{
    return tg_qr_total_codewords(version) -
           (int)tg_qr_ecc_per_block[version] * (int)tg_qr_blocks[version];
}

static void tg_qr_put(tg_qr_code *code, int x, int y, int dark, int fixed)
{
    code->modules[y * code->size + x] = (unsigned char)(dark ? 1 : 0);
    if (fixed) {
        tg_qr_fn[y * code->size + x] = 1;
    }
}

static int tg_qr_abs(int v)
{
    return v < 0 ? -v : v;
}

static int tg_qr_max(int a, int b)
{
    return a > b ? a : b;
}

static void tg_qr_finder(tg_qr_code *code, int cx, int cy)
{
    int dx;
    int dy;

    for (dy = -4; dy <= 4; ++dy) {
        for (dx = -4; dx <= 4; ++dx) {
            int dist = tg_qr_max(tg_qr_abs(dx), tg_qr_abs(dy));
            int x = cx + dx;
            int y = cy + dy;

            if (x >= 0 && x < code->size && y >= 0 && y < code->size) {
                tg_qr_put(code, x, y, dist != 2 && dist != 4, 1);
            }
        }
    }
}

static void tg_qr_alignment(tg_qr_code *code, int cx, int cy)
{
    int dx;
    int dy;

    for (dy = -2; dy <= 2; ++dy) {
        for (dx = -2; dx <= 2; ++dx) {
            tg_qr_put(code, cx + dx, cy + dy,
                      tg_qr_max(tg_qr_abs(dx), tg_qr_abs(dy)) != 1, 1);
        }
    }
}

/* The 15 format bits (level M and the mask), twice, and the dark module. */
static void tg_qr_format(tg_qr_code *code, int mask)
{
    unsigned long data = (unsigned long)mask; /* level M is 00 */
    unsigned long rem = data;
    unsigned long bits;
    int size = code->size;
    int i;

    for (i = 0; i < 10; ++i) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537UL);
    }
    bits = ((data << 10) | (rem & 0x3ffUL)) ^ 0x5412UL;
    for (i = 0; i <= 5; ++i) {
        tg_qr_put(code, 8, i, (int)((bits >> i) & 1UL), 1);
    }
    tg_qr_put(code, 8, 7, (int)((bits >> 6) & 1UL), 1);
    tg_qr_put(code, 8, 8, (int)((bits >> 7) & 1UL), 1);
    tg_qr_put(code, 7, 8, (int)((bits >> 8) & 1UL), 1);
    for (i = 9; i < 15; ++i) {
        tg_qr_put(code, 14 - i, 8, (int)((bits >> i) & 1UL), 1);
    }
    for (i = 0; i < 8; ++i) {
        tg_qr_put(code, size - 1 - i, 8, (int)((bits >> i) & 1UL), 1);
    }
    for (i = 8; i < 15; ++i) {
        tg_qr_put(code, 8, size - 15 + i, (int)((bits >> i) & 1UL), 1);
    }
    tg_qr_put(code, 8, size - 8, 1, 1);
}

/* From version 7 on, the version itself, in two 6x3 blocks. */
static void tg_qr_version_bits(tg_qr_code *code)
{
    unsigned long rem = (unsigned long)code->version;
    unsigned long bits;
    int i;

    if (code->version < 7) {
        return;
    }
    for (i = 0; i < 12; ++i) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1f25UL);
    }
    bits = ((unsigned long)code->version << 12) | (rem & 0xfffUL);
    for (i = 0; i < 18; ++i) {
        int dark = (int)((bits >> i) & 1UL);
        int a = code->size - 11 + i % 3;
        int b = i / 3;

        tg_qr_put(code, a, b, dark, 1);
        tg_qr_put(code, b, a, dark, 1);
    }
}

static void tg_qr_function_patterns(tg_qr_code *code)
{
    int count = tg_qr_align_count(code->version);
    int i;
    int j;

    for (i = 0; i < code->size; ++i) {
        tg_qr_put(code, 6, i, i % 2 == 0, 1);
        tg_qr_put(code, i, 6, i % 2 == 0, 1);
    }
    tg_qr_finder(code, 3, 3);
    tg_qr_finder(code, code->size - 4, 3);
    tg_qr_finder(code, 3, code->size - 4);
    for (i = 0; i < count; ++i) {
        for (j = 0; j < count; ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == count - 1) ||
                (i == count - 1 && j == 0)) {
                continue; /* those corners hold finders */
            }
            tg_qr_alignment(code, tg_qr_align[code->version][i],
                            tg_qr_align[code->version][j]);
        }
    }
    tg_qr_format(code, 0); /* reserves the area; the real mask comes later */
    tg_qr_version_bits(code);
}

/* Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1. */
static unsigned char tg_qr_gf_mul(unsigned char x, unsigned char y)
{
    unsigned int z = 0U;
    int i;

    for (i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11dU);
        if (((y >> i) & 1U) != 0U) {
            z ^= x;
        }
    }
    return (unsigned char)(z & 0xffU);
}

/* The Reed-Solomon generator of the given degree, highest term implied. */
static void tg_qr_rs_divisor(int degree, unsigned char *div)
{
    unsigned char root = 1U;
    int i;
    int j;

    memset(div, 0, (size_t)degree);
    div[degree - 1] = 1U;
    for (i = 0; i < degree; ++i) {
        for (j = 0; j < degree; ++j) {
            div[j] = tg_qr_gf_mul(div[j], root);
            if (j + 1 < degree) {
                div[j] ^= div[j + 1];
            }
        }
        root = tg_qr_gf_mul(root, 2U);
    }
}

static void tg_qr_rs_remainder(const unsigned char *data, int length,
                               const unsigned char *div, int degree,
                               unsigned char *out)
{
    int i;
    int j;

    memset(out, 0, (size_t)degree);
    for (i = 0; i < length; ++i) {
        unsigned char factor = (unsigned char)(data[i] ^ out[0]);

        memmove(out, out + 1, (size_t)(degree - 1));
        out[degree - 1] = 0U;
        for (j = 0; j < degree; ++j) {
            out[j] ^= tg_qr_gf_mul(div[j], factor);
        }
    }
}

static void tg_qr_put_bits(unsigned long value, int count,
                           unsigned long *bit_length)
{
    int i;

    for (i = count - 1; i >= 0; --i) {
        if (((value >> i) & 1UL) != 0UL) {
            tg_qr_data[*bit_length >> 3] |=
                (unsigned char)(0x80U >> (unsigned int)(*bit_length & 7UL));
        }
        ++*bit_length;
    }
}

/* The data codewords, then the error correction of each block, then the
   blocks interleaved as the standard reads them back. */
static int tg_qr_codewords(int version, const unsigned char *bytes,
                           unsigned long length)
{
    int data_cw = tg_qr_data_codewords(version);
    unsigned long capacity = (unsigned long)data_cw * 8UL;
    unsigned long bits = 0UL;
    int count_bits = version <= 9 ? 8 : 16;
    int blocks = (int)tg_qr_blocks[version];
    int ecc_len = (int)tg_qr_ecc_per_block[version];
    int total = tg_qr_total_codewords(version);
    int short_blocks = blocks - total % blocks;
    int short_data = total / blocks - ecc_len;
    unsigned char div[32];
    unsigned long i;
    int b;
    int k;
    int out;
    unsigned char pad = 0xecU;

    memset(tg_qr_data, 0, sizeof(tg_qr_data));
    tg_qr_put_bits(4UL, 4, &bits); /* byte mode */
    tg_qr_put_bits(length, count_bits, &bits);
    for (i = 0UL; i < length; ++i) {
        tg_qr_put_bits((unsigned long)bytes[i], 8, &bits);
    }
    if (bits > capacity) {
        return 1;
    }
    tg_qr_put_bits(0UL, (int)(capacity - bits < 4UL ? capacity - bits : 4UL),
                   &bits);
    tg_qr_put_bits(0UL, (int)((8UL - (bits & 7UL)) & 7UL), &bits);
    while (bits < capacity) {
        tg_qr_put_bits((unsigned long)pad, 8, &bits);
        pad = (unsigned char)(pad == 0xecU ? 0x11U : 0xecU);
    }
    tg_qr_rs_divisor(ecc_len, div);
    for (b = 0, k = 0; b < blocks; ++b) {
        int dl = short_data + (b < short_blocks ? 0 : 1);

        tg_qr_rs_remainder(tg_qr_data + k, dl, div, ecc_len, tg_qr_ecc[b]);
        k += dl;
    }
    out = 0;
    for (k = 0; k <= short_data; ++k) {
        int start = 0;

        for (b = 0; b < blocks; ++b) {
            int dl = short_data + (b < short_blocks ? 0 : 1);

            if (k < dl) {
                tg_qr_final[out++] = tg_qr_data[start + k];
            }
            start += dl;
        }
    }
    for (k = 0; k < ecc_len; ++k) {
        for (b = 0; b < blocks; ++b) {
            tg_qr_final[out++] = tg_qr_ecc[b][k];
        }
    }
    return out == total ? 0 : 2;
}

/* The codewords into the free modules, two columns at a time, up and down
   from the bottom right corner, stepping over the vertical timing line. */
static void tg_qr_place(tg_qr_code *code, int total)
{
    int size = code->size;
    long limit = (long)total * 8L;
    long i = 0L;
    int right;
    int vert;
    int j;

    for (right = size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;
        }
        for (vert = 0; vert < size; ++vert) {
            for (j = 0; j < 2; ++j) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;

                if (tg_qr_fn[y * size + x] == 0 && i < limit) {
                    code->modules[y * size + x] =
                        (unsigned char)((tg_qr_final[i >> 3] >>
                                         (7 - (int)(i & 7L))) & 1);
                    ++i;
                }
            }
        }
    }
}

static int tg_qr_mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return x * y % 2 + x * y % 3 == 0;
    case 6:
        return (x * y % 2 + x * y % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/* XOR, so applying a mask twice takes it off again. */
static void tg_qr_apply_mask(tg_qr_code *code, int mask)
{
    int x;
    int y;

    for (y = 0; y < code->size; ++y) {
        for (x = 0; x < code->size; ++x) {
            if (tg_qr_fn[y * code->size + x] == 0 &&
                tg_qr_mask_bit(mask, x, y)) {
                code->modules[y * code->size + x] ^= 1U;
            }
        }
    }
}

static int tg_qr_at(const tg_qr_code *code, int x, int y)
{
    return code->modules[y * code->size + x];
}

/* The four penalty rules: long runs of one colour, 2x2 blocks, patterns
   that look like a finder, and a dark share far from half. */
static long tg_qr_penalty(const tg_qr_code *code)
{
    static const unsigned char finder_a[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
    static const unsigned char finder_b[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };
    int size = code->size;
    long penalty = 0L;
    long dark = 0L;
    long total = (long)size * (long)size;
    long k;
    int axis;
    int a;
    int b;
    int i;

    for (axis = 0; axis < 2; ++axis) {
        for (a = 0; a < size; ++a) {
            int run = 1;

            for (b = 1; b < size; ++b) {
                int cur = axis == 0 ? tg_qr_at(code, b, a) : tg_qr_at(code, a, b);
                int prev = axis == 0 ? tg_qr_at(code, b - 1, a)
                                     : tg_qr_at(code, a, b - 1);

                if (cur == prev) {
                    ++run;
                } else {
                    if (run >= 5) {
                        penalty += 3L + (long)(run - 5);
                    }
                    run = 1;
                }
            }
            if (run >= 5) {
                penalty += 3L + (long)(run - 5);
            }
            for (b = 0; b + 11 <= size; ++b) {
                int match_a = 1;
                int match_b = 1;

                for (i = 0; i < 11; ++i) {
                    int v = axis == 0 ? tg_qr_at(code, b + i, a)
                                      : tg_qr_at(code, a, b + i);

                    if (v != finder_a[i]) {
                        match_a = 0;
                    }
                    if (v != finder_b[i]) {
                        match_b = 0;
                    }
                }
                penalty += 40L * (long)(match_a + match_b);
            }
        }
    }
    for (a = 0; a + 1 < size; ++a) {
        for (b = 0; b + 1 < size; ++b) {
            int v = tg_qr_at(code, b, a);

            if (v == tg_qr_at(code, b + 1, a) && v == tg_qr_at(code, b, a + 1) &&
                v == tg_qr_at(code, b + 1, a + 1)) {
                penalty += 3L;
            }
        }
    }
    for (i = 0; i < size * size; ++i) {
        dark += (long)code->modules[i];
    }
    k = dark * 20L - total * 10L;
    if (k < 0L) {
        k = -k;
    }
    k = (k + total - 1L) / total - 1L;
    penalty += k * 10L;
    return penalty;
}

int tg_qr_encode(const unsigned char *data, unsigned long length, int mask,
                 tg_qr_code *code)
{
    int version;
    int total;
    int best;
    long best_penalty;
    int m;

    if (code == 0 || (data == 0 && length != 0UL) || mask < -1 || mask > 7) {
        return 2;
    }
    if (length > (unsigned long)TG_QR_MAX_BYTES) {
        return 1;
    }
    for (version = 1; version <= TG_QR_MAX_VERSION; ++version) {
        unsigned long need =
            4UL + (version <= 9 ? 8UL : 16UL) + 8UL * length;

        if (need <= (unsigned long)tg_qr_data_codewords(version) * 8UL) {
            break;
        }
    }
    if (version > TG_QR_MAX_VERSION) {
        return 1;
    }
    memset(code, 0, sizeof(*code));
    memset(tg_qr_fn, 0, sizeof(tg_qr_fn));
    code->version = version;
    code->size = 17 + 4 * version;
    tg_qr_function_patterns(code);
    if (tg_qr_codewords(version, data, length) != 0) {
        return 1;
    }
    total = tg_qr_total_codewords(version);
    tg_qr_place(code, total);
    best = mask;
    if (best < 0) {
        best_penalty = -1L;
        for (m = 0; m < 8; ++m) {
            long p;

            tg_qr_format(code, m);
            tg_qr_apply_mask(code, m);
            p = tg_qr_penalty(code);
            if (best_penalty < 0L || p < best_penalty) {
                best_penalty = p;
                best = m;
            }
            tg_qr_apply_mask(code, m);
        }
    }
    tg_qr_format(code, best);
    tg_qr_apply_mask(code, best);
    code->mask = best;
    return 0;
}

int tg_qr_dark(const tg_qr_code *code, int x, int y)
{
    if (code == 0 || x < 0 || y < 0 || x >= code->size || y >= code->size) {
        return 0;
    }
    return code->modules[y * code->size + x] != 0;
}

void tg_qr_module_size(int room_w, int room_h, int cells, int ratio,
                       int min_w, int *mw, int *mh)
{
    int pass;
    int w;
    int h;

    if (ratio < 5 || ratio > 40) {
        ratio = 10;
    }
    if (min_w < 1) {
        min_w = 1;
    }
    for (pass = 0; pass < 2; ++pass) {
        for (w = 6; w >= min_w; --w) {
            long shown;
            long square;

            h = (w * 10 + ratio / 2) / ratio;
            if (h < 1) {
                h = 1;
            }
            shown = (long)h * (long)ratio * 100L; /* its height on screen */
            square = (long)w * 1000L;
            if (pass == 0 &&
                (shown * 100L > square * 115L || shown * 100L < square * 85L)) {
                continue;
            }
            if (w * cells <= room_w && h * cells <= room_h) {
                *mw = w;
                *mh = h;
                return;
            }
        }
    }
    *mw = min_w;
    *mh = (min_w * 10 + ratio / 2) / ratio;
    if (*mh < 1) {
        *mh = 1;
    }
}

#if !defined(TG_NO_SELFTEST)
/* FNV-1a over the modules, row by row. */
static unsigned long tg_qr_hash(const tg_qr_code *code)
{
    unsigned long h = 2166136261UL;
    int i;

    for (i = 0; i < code->size * code->size; ++i) {
        h ^= (unsigned long)code->modules[i];
        h = (h * 16777619UL) & 0xffffffffUL;
    }
    return h;
}

/* The codes below were read back by the Mac's QR reader before their sums
   were pinned here: a lane that encodes otherwise fails. They cover one
   block (version 1), two equal blocks (4), blocks of two lengths and the
   version bits (8), and the longest text, 213 bytes (10). */
static int tg_qr_module_size_self_test(void);

/* Square pixels: as wide as tall. A hires screen without interlace: 6x3,
   and where 6 does not fit 4x2, never 5x3; super-hires 4x1; nothing fits:
   the smallest. */
static int tg_qr_module_size_self_test(void)
{
    int w;
    int h;

    tg_qr_module_size(1000, 1000, 41, 10, 1, &w, &h);
    if (w != 6 || h != 6) {
        return 2;
    }
    tg_qr_module_size(1000, 1000, 41, 20, 1, &w, &h);
    if (w != 6 || h != 3) {
        return 2;
    }
    tg_qr_module_size(41 * 5, 1000, 41, 20, 1, &w, &h);
    if (w != 4 || h != 2) {
        return 2;
    }
    tg_qr_module_size(41 * 3, 1000, 41, 20, 2, &w, &h);
    if (w != 2 || h != 1) {
        return 2;
    }
    tg_qr_module_size(1000, 1000, 41, 40, 1, &w, &h);
    if (w != 4 || h != 1) {
        return 2;
    }
    tg_qr_module_size(10, 10, 41, 10, 2, &w, &h);
    if (w != 2 || h != 2) {
        return 2;
    }
    return 0;
}

int tg_qr_self_test(void)
{
    if (tg_qr_module_size_self_test() != 0) {
        puts("qr self-test: module size wrong");
        return 2;
    }
    static tg_qr_code code;
    static unsigned char long_text[TG_QR_MAX_BYTES + 1];
    static const struct {
        const char *text;
        int mask;
        int version;
        unsigned long hash;
    } pins[] = {
        { "AMIGRAM", 2, 1, 0x955c79f7UL },
        { "tg://login?token=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA", 5, 4,
          0x4fc40c83UL },
        { "tg://login?token=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyAhIiMkJSYnKCkqKywtLi8wMTIzNDU2Nzg5Ojs8PT4_QEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFlaW1xdXl8",
          7, 8, 0x170768a5UL }
    };
    unsigned int i;

    for (i = 0U; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (tg_qr_encode((const unsigned char *)pins[i].text,
                         (unsigned long)strlen(pins[i].text), pins[i].mask,
                         &code) != 0 ||
            code.version != pins[i].version ||
            code.size != 17 + 4 * pins[i].version ||
            tg_qr_hash(&code) != pins[i].hash) {
            printf("qr self-test: code %u wrong (version %d, sum %08lx)\n", i,
                   code.version, tg_qr_hash(&code));
            return 2;
        }
    }
    /* The longest text fits version 10, one byte more does not. */
    memset(long_text, 'x', sizeof(long_text));
    if (tg_qr_encode(long_text, (unsigned long)TG_QR_MAX_BYTES, 3, &code) !=
            0 ||
        code.version != 10 || tg_qr_hash(&code) != 0xb032a7e7UL ||
        tg_qr_encode(long_text, (unsigned long)TG_QR_MAX_BYTES, -1, &code) !=
            0 ||
        code.version != 10 ||
        tg_qr_encode(long_text, (unsigned long)TG_QR_MAX_BYTES + 1UL, -1,
                     &code) != 1 ||
        tg_qr_encode(long_text, 3UL, 8, &code) != 2) {
        puts("qr self-test: length limits wrong");
        return 2;
    }
    /* The quiet zone is light, the finder's corner dark. */
    if (tg_qr_encode((const unsigned char *)"A", 1UL, -1, &code) != 0 ||
        !tg_qr_dark(&code, 0, 0) || tg_qr_dark(&code, -1, 0) ||
        tg_qr_dark(&code, code.size, 3) || tg_qr_dark(&code, 7, 7)) {
        puts("qr self-test: fixed patterns wrong");
        return 2;
    }
    return 0;
}
#endif
