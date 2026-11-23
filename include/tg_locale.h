/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_LOCALE_H
#define TG_LOCALE_H

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

/* TG_STR(MSG_NAME): the user's text for MSG_NAME. */
#define TG_STR(name) tg_str((long)(name), name##_STR)

/* The language of the catalog open ("italiano"), "" when none. */
const char *tg_locale_language(void);

int tg_locale_self_test(void);

#endif
