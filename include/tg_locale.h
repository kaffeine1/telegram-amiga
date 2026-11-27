/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_LOCALE_H
#define TG_LOCALE_H

#include <stddef.h>

#include "tg_strings.h"

/* Amigram's texts in the user's language. On the Amiga the system's
   locale.library opens Catalogs/<language>/amigram.catalog next to the
   program (or in LOCALE:Catalogs), as the Locale preferences choose; the
   English texts are built in and stand wherever a catalog has none. The
   host build reads a catalog named by the TG_CATALOG environment variable,
   for the self-tests. Catalogs are Latin-1: one byte a letter. */

void tg_locale_open(void);
void tg_locale_close(void);

/* The text `id` of the catalog open, or `builtin`. Never 0. */
const char *tg_str(long id, const char *builtin);

/* TG_STR(MSG_NAME): the user's text for MSG_NAME. Built with
   -DTG_LOCALE_CHECK_FORMATS it is the English literal itself, so that the
   compiler checks the arguments of each tg_locale_format against it. */
#if defined(TG_LOCALE_CHECK_FORMATS)
#define TG_STR(name) (name##_STR)
#else
#define TG_STR(name) tg_str((long)(name), name##_STR)
#endif

/* A catalog text as UTF-8, for what is joined to Telegram's own text (a
   media label, a service line, a chat's name): that text is UTF-8 until
   the window or the console turns it into Latin-1, and a catalog letter
   above 127 would be read there as the start of a UTF-8 sequence. A text
   with no such letter comes back as it is; another one is written into
   one of a few buffers used in turn, good until four more are asked for,
   and cut at TG_LOCALE_UTF8_MAX - 1 bytes. */
#define TG_LOCALE_UTF8_MAX 640
const char *tg_locale_utf8(const char *latin1);
#if defined(TG_LOCALE_CHECK_FORMATS)
#define TG_STR_U8(name) (name##_STR)
#else
/* (the name is pasted here, not handed to TG_STR: an argument passed on
   would be replaced by its number first) */
#define TG_STR_U8(name) tg_locale_utf8(tg_str((long)(name), name##_STR))
#endif

/* sprintf of a catalog text, bounded by size: a translation may be longer
   than the English text the buffer was sized for, and what does not fit
   is cut. %s %c %d %i %u %x %X %%, with -, 0, width, .precision and l
   (the conversions scripts/catalog.py lets a text have). dst back. */
const char *tg_locale_format(char *dst, size_t size, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* The language of the catalog open ("italiano"), "" when none. */
const char *tg_locale_language(void);

int tg_locale_self_test(void);

#endif
