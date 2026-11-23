/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The signed update manifest (tg_update.h): read strictly, checked with
   RSA-2048 over the bigint code the MTProto key exchange already uses. */

#include <stdio.h>
#include <string.h>

#include "tg_update.h"
#include "tg_update_key.h"
#include "tg_mtproto_bigint.h"
#include "tg_mtproto_crypto.h"

/* DER of AlgorithmIdentifier SHA-256 inside DigestInfo, the fixed bytes
   that precede the hash in a PKCS#1 v1.5 signature (RFC 8017, 9.2). */
static const unsigned char tg_update_digest_info[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

int tg_update_signature_ok(const unsigned char *data, unsigned long length,
                           const unsigned char *signature,
                           const unsigned char *modulus)
{
    static unsigned char m[TG_MTPROTO_BIGINT_SIZE];
    static unsigned char em[TG_MTPROTO_BIGINT_SIZE];
    static const unsigned char exponent[3] = { 0x01, 0x00, 0x01 };
    unsigned char hash[32];
    int ok;

    if (data == 0 || signature == 0 || modulus == 0 ||
        tg_mtproto_bigint_cmp(signature, modulus) >= 0) {
        return 0; /* a signature is a number below the modulus */
    }
    tg_mtproto_bigint_mod_exp(signature, exponent, sizeof(exponent), modulus,
                              m);
    tg_mtproto_sha256(data, length, hash);
    /* 00 01 FF..FF 00 DigestInfo hash: 2 + 202 + 1 + 19 + 32 = 256 */
    em[0] = 0x00;
    em[1] = 0x01;
    memset(em + 2, 0xff, 202U);
    em[204] = 0x00;
    memcpy(em + 205, tg_update_digest_info, sizeof(tg_update_digest_info));
    memcpy(em + 224, hash, sizeof(hash));
    ok = memcmp(m, em, sizeof(em)) == 0;
    memset(m, 0, sizeof(m));
    return ok;
}

static int tg_update_hex(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; /* lowercase only, as openssl and shasum write it */
}

/* The word at *p up to a space or the line's end: its length, 0 when it
   is empty or longer than max - 1 or holds a character outside `allowed`
   (0 = any printable). */
static unsigned long tg_update_word(const char *p, const char *end,
                                    unsigned long max, const char *allowed)
{
    unsigned long n = 0UL;

    while (p + n < end && p[n] != ' ' && p[n] != '\n') {
        char c = p[n];
        int good = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

        if (allowed != 0 && !good && strchr(allowed, c) == 0) {
            return 0UL;
        }
        if (allowed == 0 && (c < '!' || c > '~')) {
            return 0UL;
        }
        if (++n >= max) {
            return 0UL;
        }
    }
    return n;
}

static void tg_update_copy(char *dst, const char *src, unsigned long n)
{
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

int tg_update_manifest_parse(const char *text, unsigned long length,
                             tg_update_manifest *m)
{
    static const char head[] = "Amigram update 1\n";
    const char *p;
    const char *end;
    int have_version = 0;

    if (text == 0 || m == 0) {
        return TG_UPDATE_BAD_FORMAT;
    }
    memset(m, 0, sizeof(*m));
    end = text + length;
    if (length < sizeof(head) - 1U ||
        memcmp(text, head, sizeof(head) - 1U) != 0) {
        return TG_UPDATE_BAD_FORMAT;
    }
    p = text + sizeof(head) - 1U;
    while (p < end) {
        const char *eol = p;
        unsigned long n;

        while (eol < end && *eol != '\n') {
            ++eol;
        }
        if (eol - p > 7 && memcmp(p, "version ", 8U) == 0) {
            if (have_version) {
                return TG_UPDATE_BAD_FORMAT;
            }
            n = tg_update_word(p + 8, eol, TG_UPDATE_VERSION_MAX, ".");
            if (n == 0UL || p + 8 + n != eol || (p[8] < '0' || p[8] > '9')) {
                return TG_UPDATE_BAD_FORMAT;
            }
            tg_update_copy(m->version, p + 8, n);
            have_version = 1;
        } else if (eol - p > 9 && memcmp(p, "signature ", 10U) == 0) {
            const char *h = p + 10;
            unsigned long i;

            if (eol - h != 2 * TG_UPDATE_SIGNATURE_LENGTH ||
                (eol < end && eol + 1 != end)) {
                return TG_UPDATE_BAD_FORMAT; /* the last line, exactly */
            }
            for (i = 0UL; i < TG_UPDATE_SIGNATURE_LENGTH; ++i) {
                int hi = tg_update_hex((unsigned char)h[2 * i]);
                int lo = tg_update_hex((unsigned char)h[2 * i + 1]);

                if (hi < 0 || lo < 0) {
                    return TG_UPDATE_BAD_FORMAT;
                }
                m->signature[i] = (unsigned char)(hi * 16 + lo);
            }
            m->signed_length = (unsigned long)(p - text);
            return have_version && m->count > 0U ? TG_UPDATE_OK
                                                 : TG_UPDATE_BAD_FORMAT;
        } else {
            /* <lane> <file> <size> <sha256> */
            tg_update_entry *e;
            const char *q = p;
            unsigned long size = 0UL;
            unsigned int i;

            if (m->count >= TG_UPDATE_LANES_MAX) {
                return TG_UPDATE_BAD_FORMAT;
            }
            e = &m->entries[m->count];
            n = tg_update_word(q, eol, TG_UPDATE_LANE_MAX, "-_");
            if (n == 0UL || q + n >= eol || q[n] != ' ') {
                return TG_UPDATE_BAD_FORMAT;
            }
            tg_update_copy(e->lane, q, n);
            q += n + 1;
            n = tg_update_word(q, eol, TG_UPDATE_FILE_NAME_MAX,
                               "-_.ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            if (n == 0UL || q + n >= eol || q[n] != ' ') {
                return TG_UPDATE_BAD_FORMAT;
            }
            tg_update_copy(e->file_name, q, n);
            q += n + 1;
            for (n = 0UL; q + n < eol && q[n] >= '0' && q[n] <= '9'; ++n) {
                if (n >= 9UL) {
                    return TG_UPDATE_BAD_FORMAT;
                }
                size = size * 10UL + (unsigned long)(q[n] - '0');
            }
            if (n == 0UL || q + n >= eol || q[n] != ' ' || size == 0UL ||
                size > TG_UPDATE_FILE_SIZE_MAX) {
                return TG_UPDATE_BAD_FORMAT;
            }
            e->size = size;
            q += n + 1;
            if (eol - q != 64) {
                return TG_UPDATE_BAD_FORMAT;
            }
            for (n = 0UL; n < 64UL; ++n) {
                if (tg_update_hex((unsigned char)q[n]) < 0) {
                    return TG_UPDATE_BAD_FORMAT;
                }
            }
            tg_update_copy(e->sha256_hex, q, 64UL);
            for (i = 0U; i < m->count; ++i) {
                if (strcmp(m->entries[i].lane, e->lane) == 0) {
                    return TG_UPDATE_BAD_FORMAT; /* a platform twice */
                }
            }
            ++m->count;
        }
        p = eol + 1;
    }
    return TG_UPDATE_BAD_FORMAT; /* no signature */
}

int tg_update_manifest_read(const char *text, unsigned long length,
                            tg_update_manifest *m)
{
    int rc = tg_update_manifest_parse(text, length, m);

    if (rc != TG_UPDATE_OK) {
        return rc;
    }
    return tg_update_signature_ok((const unsigned char *)text,
                                  m->signed_length, m->signature,
                                  tg_update_release_modulus)
               ? TG_UPDATE_OK
               : TG_UPDATE_BAD_SIGNATURE;
}

const tg_update_entry *tg_update_manifest_lane(const tg_update_manifest *m,
                                               const char *lane)
{
    unsigned int i;

    if (m == 0 || lane == 0) {
        return 0;
    }
    for (i = 0U; i < m->count; ++i) {
        if (strcmp(m->entries[i].lane, lane) == 0) {
            return &m->entries[i];
        }
    }
    return 0;
}

const char *tg_update_lane(void)
{
#if defined(__amigaos4__)
    return "os4";
#elif defined(__MORPHOS__) || defined(__MORPHOS)
    return "morphos";
#elif defined(__AROS__) && defined(__aarch64__)
    return "aros-aarch64";
#elif defined(__AROS__) && defined(__x86_64__)
    return "aros-x86_64";
#elif defined(__AROS__)
    return "aros-i386";
#elif defined(__amigaos3__) && defined(TG_NO_GUI)
    return "os3-68000";
#elif defined(__amigaos3__)
    return "os3";
#else
    return "host";
#endif
}

void tg_update_manifest_name(const char *version, char *out,
                             unsigned long out_size)
{
    if (out == 0 || out_size == 0UL) {
        return;
    }
    out[0] = '\0';
    if (version != 0 && strlen(version) + 24U < out_size) {
        sprintf(out, "Amigram-update-%s.txt", version);
    }
}

#if !defined(TG_NO_SELFTEST)
/* A manifest signed with a throwaway key made for this test alone (its
   private half was never kept): the signature holds, and goes wrong at a
   byte changed in the text, in the signature or in the key; the parser
   takes the good form and refuses the bad ones. */
int tg_update_self_test(void)
{
    static const char signed_text[] =
        "Amigram update 1\n"
        "version 9.9\n"
        "os3 Amigram-9.9-os3 3 "
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
    static const unsigned char test_modulus[256] = {
        0xde, 0x2f, 0x01, 0x44, 0x9b, 0xeb, 0xc6, 0xf5, 0xde, 0x9f, 0x68, 0xcd,
        0xca, 0xcd, 0x6b, 0xc0, 0x74, 0x4c, 0x9f, 0x71, 0x08, 0xa6, 0xa9, 0xa5,
        0x9c, 0x4d, 0x20, 0xc7, 0x56, 0x7e, 0x74, 0xb3, 0x4a, 0xa9, 0xe8, 0x05,
        0x0f, 0x2e, 0x92, 0x7f, 0x4f, 0x7e, 0xad, 0x44, 0x92, 0x96, 0xc0, 0xfb,
        0x13, 0x77, 0x2d, 0x04, 0x2e, 0x09, 0x6e, 0xba, 0xfc, 0xdb, 0x1d, 0x6a,
        0xf2, 0xb9, 0x61, 0x46, 0x90, 0x1a, 0x0d, 0x44, 0x0a, 0xd4, 0xcc, 0xc9,
        0x2e, 0x1f, 0xcb, 0xb5, 0xd0, 0x15, 0x2a, 0xa1, 0x6b, 0x51, 0x3e, 0x79,
        0xca, 0xdf, 0x8b, 0xfb, 0x2b, 0x12, 0x33, 0xcf, 0x08, 0x4d, 0xe2, 0x10,
        0x6d, 0xdb, 0x3a, 0x1d, 0xea, 0xd9, 0xa4, 0xa2, 0xda, 0x45, 0x94, 0xe1,
        0x71, 0x3b, 0x32, 0x4c, 0xcb, 0x5d, 0xe0, 0xf3, 0x6a, 0x85, 0xe6, 0xa4,
        0x3f, 0x21, 0x71, 0x80, 0xa0, 0xa3, 0x45, 0x8f, 0x4f, 0xd8, 0x76, 0x23,
        0x2c, 0x3b, 0x67, 0xcd, 0xc2, 0xdf, 0x3a, 0x7a, 0x56, 0x24, 0x02, 0xe5,
        0xfc, 0x2e, 0x41, 0x9b, 0x67, 0x06, 0x1c, 0x3e, 0xc2, 0x7c, 0x00, 0x78,
        0x64, 0xaf, 0x92, 0xc7, 0xcb, 0x15, 0x1b, 0xac, 0xdb, 0xe4, 0x33, 0xef,
        0xc9, 0x12, 0x98, 0xfa, 0x7f, 0xe1, 0xf6, 0x10, 0x06, 0xf3, 0xa6, 0xa3,
        0xe5, 0x4c, 0xa1, 0xa5, 0xe6, 0x4f, 0x9f, 0x71, 0x96, 0x0e, 0xe0, 0xb7,
        0xaf, 0x37, 0x5b, 0x1f, 0x7f, 0xf5, 0xd0, 0x67, 0x28, 0x37, 0x41, 0x7c,
        0x01, 0x94, 0x64, 0x30, 0xaa, 0xeb, 0x10, 0xf8, 0xa2, 0xf7, 0x99, 0x7b,
        0x96, 0x12, 0x98, 0x27, 0x6f, 0xd7, 0xf2, 0x7b, 0xfa, 0x8f, 0x56, 0x44,
        0xad, 0x2a, 0xd0, 0x57, 0x12, 0xde, 0x0a, 0x0b, 0x8b, 0x75, 0xc8, 0xdb,
        0xf4, 0x5f, 0x63, 0x20, 0xa0, 0x7c, 0x7b, 0x57, 0x38, 0xbc, 0xc2, 0x6f,
        0x12, 0x40, 0xa4, 0x63
    };
    static const char test_signature_hex[] =
        "94733496699f561318965e2e2840b544efdfd1bd6d90293440923d047e965f9c"
        "a3a476d735a901f937a90d9e873627eaeb586530de366d10396c20ec43c9861f"
        "23e78f91a2cad1e9f7f2cefa99096fb7b1e8faea74ed614515ef29b747c2ad7a"
        "057d051585e20b75a185d6ef64b94c88e5f767bfb372bff4a7393c871b497636"
        "d4db906fef2eecd28a6d886710ab1fd14afb1d31c73472e5e07c4875da61a0d1"
        "efebfbfe1ff50030202dff0d5f1891b99c70705ef0f2b3d4aeed26bcb2c52e46"
        "82bc6dd915220a2d7e84a1e00711d9675e88cfb5320ff8c1c2579c1cf76e75a7"
        "7f7ba09f2d9c43bbe183a25c69700a70631d53165487d70922d21ee8c9c57199";
    static char text[sizeof(signed_text) + 600];
    static tg_update_manifest m;
    static unsigned char key[256];
    static const char *const bad[] = {
        "Amigram update 2\nversion 9.9\n",
        "Amigram update 1\nversion 9.9\nos3 A 3 "
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD\n",
        "Amigram update 1\nversion 9.9\nos3 A 0 "
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n",
        "Amigram update 1\nversion x\nos3 A 3 "
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n",
        "Amigram update 1\nversion 9.9\nos3 A/B 3 "
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n",
        "Amigram update 1\nversion 9.9\nos3 A 3 "
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
        "os3 B 3 "
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
    };
    unsigned long n;
    unsigned int i;
    const tg_update_entry *e;

    n = (unsigned long)(sizeof(signed_text) - 1U);
    if (tg_update_signature_ok((const unsigned char *)signed_text, n, 0,
                               test_modulus)) {
        puts("update self-test: a missing signature passed");
        return 2;
    }
    sprintf(text, "%ssignature %s\n", signed_text, test_signature_hex);
    if (tg_update_manifest_parse(text, (unsigned long)strlen(text), &m) !=
            TG_UPDATE_OK ||
        strcmp(m.version, "9.9") != 0 || m.count != 1U ||
        m.signed_length != n ||
        (e = tg_update_manifest_lane(&m, "os3")) == 0 || e->size != 3UL ||
        strcmp(e->file_name, "Amigram-9.9-os3") != 0 ||
        tg_update_manifest_lane(&m, "os4") != 0) {
        puts("update self-test: the manifest was misread");
        return 2;
    }
    if (!tg_update_signature_ok((const unsigned char *)text, m.signed_length,
                                m.signature, test_modulus)) {
        puts("update self-test: a good signature was refused");
        return 2;
    }
    text[20] = (char)(text[20] ^ 1); /* the version's digit */
    if (tg_update_signature_ok((const unsigned char *)text, m.signed_length,
                               m.signature, test_modulus)) {
        puts("update self-test: a changed manifest passed");
        return 2;
    }
    text[20] = (char)(text[20] ^ 1);
    m.signature[100] ^= 0x01U;
    if (tg_update_signature_ok((const unsigned char *)text, m.signed_length,
                               m.signature, test_modulus)) {
        puts("update self-test: a changed signature passed");
        return 2;
    }
    m.signature[100] ^= 0x01U;
    memcpy(key, test_modulus, sizeof(key));
    key[200] ^= 0x01U;
    if (tg_update_signature_ok((const unsigned char *)text, m.signed_length,
                               m.signature, key)) {
        puts("update self-test: another key passed");
        return 2;
    }
    /* signed by the test key, so not by the release key */
    if (tg_update_manifest_read(text, (unsigned long)strlen(text), &m) !=
        TG_UPDATE_BAD_SIGNATURE) {
        puts("update self-test: the release key took a test signature");
        return 2;
    }
    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        sprintf(text, "%ssignature %s\n", bad[i], test_signature_hex);
        if (tg_update_manifest_parse(text, (unsigned long)strlen(text),
                                     &m) == TG_UPDATE_OK) {
            printf("update self-test: bad manifest %u taken\n", i);
            return 2;
        }
    }
    /* no signature line at all, and text after it */
    if (tg_update_manifest_parse(signed_text, n, &m) == TG_UPDATE_OK) {
        puts("update self-test: an unsigned manifest taken");
        return 2;
    }
    sprintf(text, "%ssignature %s\nextra\n", signed_text, test_signature_hex);
    if (tg_update_manifest_parse(text, (unsigned long)strlen(text), &m) ==
        TG_UPDATE_OK) {
        puts("update self-test: text after the signature taken");
        return 2;
    }
    tg_update_manifest_name("0.7", text, 40UL);
    if (strcmp(text, "Amigram-update-0.7.txt") != 0) {
        puts("update self-test: manifest name wrong");
        return 2;
    }
    return 0;
}
#endif
