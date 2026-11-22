/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_QR_H
#define TG_QR_H

#include <stdio.h>

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

/* The text client's login code: a small window on the Workbench screen
   that never takes the keyboard (Amiga), the terminal (the host build).
   Each call with a new code redraws it; close takes it away. 0 = shown,
   2 = no window could open (an OS older than 3.0, no public screen). */
int tg_qr_view_show(const tg_qr_code *code, FILE *stream);
void tg_qr_view_close(void);

/* The size of a module on screen for a code of `cells` modules a side
   (quiet zone in) in room_w x room_h pixels: min_w to 6 pixels wide, and
   as tall as that shows on a screen whose pixels are ratio/10 times
   taller than wide (20 on a hires screen without interlace), so the code
   looks square. A width whose height cannot match within 15% (5x3 on such
   a screen would show 20% too tall) is passed over while a smaller one
   fits; only when none does is the nearest taken. */
void tg_qr_module_size(int room_w, int room_h, int cells, int ratio,
                       int min_w, int *mw, int *mh);

/* How much taller than wide the pixels of `screen` (a struct Screen *)
   are, times ten, from the display database, for the chipset's own modes
   only (the monitors up to DblPAL): an RTG driver reckons for a 4:3
   monitor whatever the panel (an OS4 1920x1080 screen said 14/10) and its
   pixels are square, so anything else is 10. *mode, when given, gets the
   screen's mode ID. 10 off the Amiga. */
int tg_qr_screen_ratio(void *screen, unsigned long *mode);

int tg_qr_self_test(void);

#endif
