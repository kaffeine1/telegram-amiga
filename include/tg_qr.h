/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_QR_H
#define TG_QR_H

/* A QR Code (ISO/IEC 18004) for the login screen: byte mode, error
   correction level M, versions 1 to 10, so up to 213 bytes of text. The
   login link Telegram hands out (tg://login?token=...) needs version 4 to
   7. */

#define TG_QR_MAX_VERSION 10
#define TG_QR_MAX_SIZE (17 + 4 * TG_QR_MAX_VERSION) /* 57 modules a side */
#define TG_QR_MAX_BYTES 213

typedef struct tg_qr_code {
    int version;  /* 1..10 */
    int size;     /* modules a side: 17 + 4 * version */
    int mask;     /* 0..7, the one with the lowest penalty unless forced */
    unsigned char modules[TG_QR_MAX_SIZE * TG_QR_MAX_SIZE]; /* 1 = dark */
} tg_qr_code;

/* Encodes `length` bytes of `data` into `code`. mask: 0..7 forces that
   mask (for tests), -1 picks the best by the standard's penalty rules.
   0 = done, 1 = too long for version 10, 2 = bad arguments. */
int tg_qr_encode(const unsigned char *data, unsigned long length, int mask,
                 tg_qr_code *code);

/* 1 when the module at column x, row y is dark; 0 outside the code, which
   is the light quiet zone around it. */
int tg_qr_dark(const tg_qr_code *code, int x, int y);

int tg_qr_self_test(void);

#endif
