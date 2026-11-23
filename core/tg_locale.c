/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Amigram's texts in the user's language (tg_locale.h): locale.library's
   catalog on the Amiga, a catalog file read by hand on the host. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_locale.h"

#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__AROS__)
#define TG_LOCALE_AMIGA 1
#endif

#if defined(TG_LOCALE_AMIGA)

#if defined(__amigaos4__)
#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif
#endif
#include <exec/types.h>
#include <libraries/locale.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/locale.h>

/* The base the SDK declares, of the type it declares it with (it differs
   from one system to the next). */
__typeof__(LocaleBase) LocaleBase = 0;
#if defined(__amigaos4__)
struct LocaleIFace *ILocale = 0;
#endif

static struct Catalog *tg_locale_catalog = 0;
static char tg_locale_lang[32];

void tg_locale_open(void)
{
    struct TagItem tags[2];

    if (tg_locale_catalog != 0) {
        return;
    }
#if defined(__amigaos4__)
    LocaleBase = OpenLibrary("locale.library", 50L);
    if (LocaleBase == 0) {
        return;
    }
    ILocale = (struct LocaleIFace *)GetInterface(LocaleBase, "main", 1L, 0);
    if (ILocale == 0) {
        CloseLibrary(LocaleBase);
        LocaleBase = 0;
        return;
    }
#else
    LocaleBase = (__typeof__(LocaleBase))OpenLibrary(
        (CONST_STRPTR)"locale.library", 38L);
    if (LocaleBase == 0) {
        return;
    }
#endif
    tags[0].ti_Tag = OC_BuiltInLanguage;
#if defined(__AROS__)
    tags[0].ti_Data = (IPTR)"english";
#else
    tags[0].ti_Data = (ULONG)"english";
#endif
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0;
    tg_locale_catalog = OpenCatalogA(0, (CONST_STRPTR)"amigram.catalog",
                                     tags);
    tg_locale_lang[0] = '\0';
    if (tg_locale_catalog != 0 && tg_locale_catalog->cat_Language != 0) {
        strncpy(tg_locale_lang, (const char *)tg_locale_catalog->cat_Language,
                sizeof(tg_locale_lang) - 1U);
        tg_locale_lang[sizeof(tg_locale_lang) - 1U] = '\0';
    }
}

void tg_locale_close(void)
{
    if (tg_locale_catalog != 0) {
        CloseCatalog(tg_locale_catalog);
        tg_locale_catalog = 0;
    }
#if defined(__amigaos4__)
    if (ILocale != 0) {
        DropInterface((struct Interface *)ILocale);
        ILocale = 0;
    }
#endif
    if (LocaleBase != 0) {
        CloseLibrary((struct Library *)LocaleBase);
        LocaleBase = 0;
    }
    tg_locale_lang[0] = '\0';
}

const char *tg_str(long id, const char *builtin)
{
    const char *s;

    if (tg_locale_catalog == 0) {
        return builtin;
    }
    s = (const char *)GetCatalogStr(tg_locale_catalog, (LONG)id,
                                    (CONST_STRPTR)builtin);
    return s != 0 ? s : builtin;
}

const char *tg_locale_language(void)
{
    return tg_locale_lang;
}

#else /* the host: a catalog file read by hand, for the self-tests */

#define TG_LOCALE_HOST_MAX 2048

static unsigned char *tg_locale_data = 0;
static const char *tg_locale_texts[TG_LOCALE_HOST_MAX];
static char tg_locale_lang[32];

static unsigned long tg_locale_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* Reads the IFF CTLG at path into the table; 0 = read. */
static int tg_locale_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned long end;
    unsigned long i;

    if (f == 0) {
        return 2;
    }
    if (fseek(f, 0L, SEEK_END) != 0 || (size = ftell(f)) < 12L ||
        size > 1048576L || fseek(f, 0L, SEEK_SET) != 0) {
        fclose(f);
        return 2;
    }
    tg_locale_data = (unsigned char *)malloc((size_t)size + 1U);
    if (tg_locale_data == 0 ||
        fread(tg_locale_data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(tg_locale_data);
        tg_locale_data = 0;
        return 2;
    }
    fclose(f);
    tg_locale_data[size] = 0;
    if (memcmp(tg_locale_data, "FORM", 4U) != 0 ||
        memcmp(tg_locale_data + 8, "CTLG", 4U) != 0) {
        free(tg_locale_data);
        tg_locale_data = 0;
        return 2;
    }
    end = 8UL + tg_locale_be32(tg_locale_data + 4);
    if (end > (unsigned long)size) {
        end = (unsigned long)size;
    }
    i = 12UL;
    while (i + 8UL <= end) {
        unsigned long len = tg_locale_be32(tg_locale_data + i + 4);
        const unsigned char *body = tg_locale_data + i + 8;

        if (i + 8UL + len > end) {
            break;
        }
        if (memcmp(tg_locale_data + i, "LANG", 4U) == 0 && len > 0UL) {
            unsigned long n = len < sizeof(tg_locale_lang) - 1U
                                  ? len
                                  : sizeof(tg_locale_lang) - 1U;

            memcpy(tg_locale_lang, body, (size_t)n);
            tg_locale_lang[n] = '\0';
        } else if (memcmp(tg_locale_data + i, "STRS", 4U) == 0) {
            unsigned long j = 0UL;

            while (j + 8UL <= len) {
                unsigned long id = tg_locale_be32(body + j);
                unsigned long n = tg_locale_be32(body + j + 4);

                if (j + 8UL + n > len) {
                    break;
                }
                if (id < TG_LOCALE_HOST_MAX && n > 0UL &&
                    body[j + 8UL + n - 1UL] == 0) {
                    tg_locale_texts[id] = (const char *)(body + j + 8);
                }
                j += 8UL + ((n + 3UL) & ~3UL);
            }
        }
        i += 8UL + len + (len & 1UL);
    }
    return 0;
}

void tg_locale_open(void)
{
    const char *path = getenv("TG_CATALOG");

    if (tg_locale_data == 0 && path != 0 && path[0] != '\0') {
        (void)tg_locale_load(path);
    }
}

void tg_locale_close(void)
{
    memset((void *)tg_locale_texts, 0, sizeof(tg_locale_texts));
    free(tg_locale_data);
    tg_locale_data = 0;
    tg_locale_lang[0] = '\0';
}

const char *tg_str(long id, const char *builtin)
{
    if (id >= 0L && id < TG_LOCALE_HOST_MAX && tg_locale_texts[id] != 0) {
        return tg_locale_texts[id];
    }
    return builtin;
}

const char *tg_locale_language(void)
{
    return tg_locale_lang;
}

#endif

#if !defined(TG_NO_SELFTEST)
/* Without a catalog every text is its English one. */
int tg_locale_self_test(void)
{
    if (strcmp(TG_STR(MSG_MENU_QUIT), MSG_MENU_QUIT_STR) != 0 &&
        tg_locale_language()[0] == '\0') {
        puts("locale self-test: a text changed without a catalog");
        return 2;
    }
    if (tg_str(999999L, "fallback") == 0 ||
        strcmp(tg_str(999999L, "fallback"), "fallback") != 0) {
        puts("locale self-test: an unknown id lost its fallback");
        return 2;
    }
    return 0;
}
#endif
