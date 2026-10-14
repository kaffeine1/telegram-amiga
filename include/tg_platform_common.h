/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Shared internal helpers for the AmigaOS 3.x, AmigaOS 4.x, MorphOS and AROS
 * platform backends: IPv4 literal parsing without DNS, block-to-KB arithmetic,
 * error string formatting, and the SHA-256 Hash-DRBG with persistent seed
 * ratcheting.
 */
#ifndef TG_PLATFORM_COMMON_H
#define TG_PLATFORM_COMMON_H

#include <stdio.h>
#include <string.h>

#include "tg_file.h"
#include "tg_mtproto_crypto.h"

#if defined(__GNUC__)
#define TG_PLATFORM_UNUSED __attribute__((unused))
#else
#define TG_PLATFORM_UNUSED
#endif

static TG_PLATFORM_UNUSED void tg_platform_set_error(
    char *error_buffer,
    unsigned long error_buffer_size,
    const char *message)
{
    if (error_buffer != 0 && error_buffer_size > 0) {
        strncpy(error_buffer, message, error_buffer_size - 1);
        error_buffer[error_buffer_size - 1] = '\0';
    }
}

/* Parse a dotted-quad IPv4 literal ("a.b.c.d") into 4 network-order bytes
   without calling gethostbyname() or inet_addr(). Returns 1 on a valid IPv4
   literal, 0 otherwise. */
static TG_PLATFORM_UNUSED int tg_platform_parse_ipv4(const char *host,
                                                     unsigned char out[4])
{
    int i;
    const char *p = host;

    if (p == 0 || *p == '\0' || out == 0) {
        return 0;
    }
    for (i = 0; i < 4; ++i) {
        unsigned long val = 0UL;
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            val = val * 10UL + (unsigned long)(*p - '0');
            if (val > 255UL) {
                return 0;
            }
            ++p;
            ++digits;
        }
        if (digits == 0) {
            return 0;
        }
        out[i] = (unsigned char)val;
        if (i < 3) {
            if (*p != '.') {
                return 0;
            }
            ++p;
        }
    }
    return *p == '\0';
}

/* Free blocks to KB without overflowing 32 bits on a big volume. */
static TG_PLATFORM_UNUSED unsigned long tg_platform_blocks_kb(
    unsigned long blocks,
    unsigned long bytes_per_block)
{
    if (bytes_per_block >= 1024UL) {
        unsigned long per = bytes_per_block / 1024UL;

        return blocks > ~0UL / per ? ~0UL : blocks * per;
    }
    return bytes_per_block == 0UL ? 0UL : blocks / (1024UL / bytes_per_block);
}

static TG_PLATFORM_UNUSED unsigned long tg_platform_drbg_calls = 0;

/* Shared SHA-256 Hash-DRBG core: mixes the gathered hardware pool with
   PROGDIR:data/telegram-seed.bin, generates output blocks, ratchets the
   state, and persists a fresh 64-byte seed on the first seed pass. */
static TG_PLATFORM_UNUSED void tg_platform_drbg_step(
    unsigned char state[TG_MTPROTO_SHA256_LENGTH],
    const void *extra, unsigned long extra_len,
    unsigned char *out, unsigned long n)
{
    tg_mtproto_sha256_ctx ctx;
    unsigned char key[TG_MTPROTO_SHA256_LENGTH];
    unsigned char block[TG_MTPROTO_SHA256_LENGTH];
    unsigned long off;
    unsigned long ctr;

    ++tg_platform_drbg_calls;
    tg_mtproto_sha256_init(&ctx);
    tg_mtproto_sha256_update(&ctx, state, TG_MTPROTO_SHA256_LENGTH);
    tg_mtproto_sha256_update(&ctx,
                             (const unsigned char *)&tg_platform_drbg_calls,
                             sizeof(tg_platform_drbg_calls));
    if (extra != 0 && extra_len > 0UL) {
        tg_mtproto_sha256_update(&ctx, (const unsigned char *)extra, extra_len);
    }
    tg_mtproto_sha256_final(&ctx, key);

    off = 0UL;
    ctr = 0UL;
    while (off < n) {
        unsigned char cb[TG_MTPROTO_SHA256_LENGTH + 4];
        unsigned long take;

        memcpy(cb, key, TG_MTPROTO_SHA256_LENGTH);
        cb[TG_MTPROTO_SHA256_LENGTH + 0] = (unsigned char)((ctr >> 24) & 0xffUL);
        cb[TG_MTPROTO_SHA256_LENGTH + 1] = (unsigned char)((ctr >> 16) & 0xffUL);
        cb[TG_MTPROTO_SHA256_LENGTH + 2] = (unsigned char)((ctr >> 8) & 0xffUL);
        cb[TG_MTPROTO_SHA256_LENGTH + 3] = (unsigned char)(ctr & 0xffUL);
        tg_mtproto_sha256(cb, sizeof(cb), block);
        take = n - off;
        if (take > TG_MTPROTO_SHA256_LENGTH) {
            take = TG_MTPROTO_SHA256_LENGTH;
        }
        memcpy(out + off, block, (size_t)take);
        off += take;
        ++ctr;
    }

    {
        unsigned char rb[TG_MTPROTO_SHA256_LENGTH + 1];

        memcpy(rb, key, TG_MTPROTO_SHA256_LENGTH);
        rb[TG_MTPROTO_SHA256_LENGTH] = 0x01;
        tg_mtproto_sha256(rb, sizeof(rb), state);
    }
    memset(&ctx, 0, sizeof(ctx));
    memset(key, 0, sizeof(key));
    memset(block, 0, sizeof(block));
}

static TG_PLATFORM_UNUSED void tg_platform_drbg_seed_pool(
    unsigned char state[TG_MTPROTO_SHA256_LENGTH],
    int *ready,
    unsigned char *pool,
    unsigned long pool_cap,
    unsigned long gathered_len)
{
    unsigned long len = gathered_len;
    FILE *seed_file = tg_file_open_seed("rb");

    if (seed_file != 0) {
        unsigned char saved[64];
        unsigned long got = (unsigned long)fread(saved, 1U, sizeof(saved),
                                                 seed_file);
        fclose(seed_file);
        if (got > 0UL && len + got <= pool_cap) {
            memcpy(pool + len, saved, (size_t)got);
            len += got;
        }
        memset(saved, 0, sizeof(saved));
    }
    tg_mtproto_sha256(pool, len, state);
    *ready = 1;
    {
        unsigned char fresh[64];
        FILE *out_seed;

        tg_platform_drbg_step(state, &len, sizeof(len), fresh, sizeof(fresh));
        out_seed = tg_file_open_seed("wb");
        if (out_seed != 0) {
            (void)fwrite(fresh, 1U, sizeof(fresh), out_seed);
            fclose(out_seed);
        }
        memset(fresh, 0, sizeof(fresh));
    }
}

#endif /* TG_PLATFORM_COMMON_H */
