/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Amigram's texts in the user's language (tg_locale.h): locale.library's
   catalog on the Amiga, a catalog file read by hand on the host. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_locale.h"

#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__AROS__)
#define TG_LOCALE_AMIGA 1
#endif

/* A catalog read by hand: the host's way for the self-tests, and the
   Amiga's when locale.library does not find the catalog in the program's
   drawer (AmigaOS 3.2's locale.library 47 looks in LOCALE:Catalogs only:
   on a Vampire the window stayed English with Catalogs/italiano/ beside
   the program, where AmigaOS 4 and MorphOS find it). The table is made
   when a catalog is read, so a system without one spends nothing. */
#define TG_LOCALE_TEXTS_MAX (TG_STRINGS_ID_MAX + 1)

static unsigned char *tg_locale_data = 0;
static const char **tg_locale_texts = 0;
static char tg_locale_lang[32];

static unsigned long tg_locale_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void tg_locale_hand_free(void)
{
    free((void *)tg_locale_texts);
    tg_locale_texts = 0;
    free(tg_locale_data);
    tg_locale_data = 0;
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
    tg_locale_texts = (const char **)calloc((size_t)TG_LOCALE_TEXTS_MAX,
                                            sizeof(const char *));
    if (tg_locale_data == 0 || tg_locale_texts == 0 ||
        fread(tg_locale_data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        tg_locale_hand_free();
        return 2;
    }
    fclose(f);
    tg_locale_data[size] = 0;
    if (memcmp(tg_locale_data, "FORM", 4U) != 0 ||
        memcmp(tg_locale_data + 8, "CTLG", 4U) != 0) {
        tg_locale_hand_free();
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
                if (id < TG_LOCALE_TEXTS_MAX && n > 0UL &&
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

static const char *tg_locale_hand_str(long id)
{
    if (tg_locale_texts != 0 && id >= 0L && id < TG_LOCALE_TEXTS_MAX) {
        return tg_locale_texts[id];
    }
    return 0;
}

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

/* No catalog from locale.library: the program's own drawer, by hand, in
   the user's languages in their order, English ending the search. */
static void tg_locale_open_by_hand(void)
{
    struct Locale *loc = OpenLocale(0);
    char path[96];
    int i;

    if (loc == 0) {
        return;
    }
    for (i = 0; i < 10 && loc->loc_PrefLanguages[i] != 0; ++i) {
        const char *lang = (const char *)loc->loc_PrefLanguages[i];

        if (strcmp(lang, "english") == 0 || strlen(lang) > 40U) {
            break;
        }
        sprintf(path, "PROGDIR:Catalogs/%s/amigram.catalog", lang);
        if (tg_locale_load(path) == 0) {
            break;
        }
    }
    CloseLocale(loc);
}

void tg_locale_open(void)
{
    struct TagItem tags[2];

    if (tg_locale_catalog != 0 || tg_locale_texts != 0) {
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
    } else if (tg_locale_catalog == 0) {
        tg_locale_open_by_hand();
    }
}

void tg_locale_close(void)
{
    if (tg_locale_catalog != 0) {
        CloseCatalog(tg_locale_catalog);
        tg_locale_catalog = 0;
    }
    tg_locale_hand_free();
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
        s = tg_locale_hand_str(id);
        return s != 0 ? s : builtin;
    }
    s = (const char *)GetCatalogStr(tg_locale_catalog, (LONG)id,
                                    (CONST_STRPTR)builtin);
    return s != 0 ? s : builtin;
}

#else /* the host: the catalog TG_CATALOG names, for the self-tests */

void tg_locale_open(void)
{
    const char *path = getenv("TG_CATALOG");

    if (tg_locale_data == 0 && path != 0 && path[0] != '\0') {
        (void)tg_locale_load(path);
    }
}

void tg_locale_close(void)
{
    tg_locale_hand_free();
    tg_locale_lang[0] = '\0';
}

const char *tg_str(long id, const char *builtin)
{
    const char *s = tg_locale_hand_str(id);

    return s != 0 ? s : builtin;
}

#endif

const char *tg_locale_language(void)
{
    return tg_locale_lang;
}

const char *tg_locale_utf8(const char *latin1)
{
    static char ring[4][TG_LOCALE_UTF8_MAX];
    static int next;
    const unsigned char *p = (const unsigned char *)latin1;
    char *out;
    size_t n = 0U;

    if (latin1 == 0) {
        return "";
    }
    while (*p != '\0' && *p < 0x80U) {
        ++p;
    }
    if (*p == '\0') {
        return latin1; /* plain ASCII is the same in both */
    }
    out = ring[next];
    next = (next + 1) % 4;
    for (p = (const unsigned char *)latin1; *p != '\0'; ++p) {
        if (*p < 0x80U) {
            if (n + 1U >= (size_t)TG_LOCALE_UTF8_MAX) {
                break;
            }
            out[n++] = (char)*p;
        } else {
            if (n + 2U >= (size_t)TG_LOCALE_UTF8_MAX) {
                break; /* never half a letter */
            }
            out[n++] = (char)(0xc0U | (*p >> 6));
            out[n++] = (char)(0x80U | (*p & 0x3fU));
        }
    }
    out[n] = '\0';
    return out;
}

/* tg_locale_format: sprintf bounded by size, for the texts of a catalog,
   which may be longer than the English ones a buffer was sized for. Ours,
   since snprintf is not on every system's C library alike: %s %c %d %i %u
   %x %X %%, with -, 0, width, .precision and l. What does not fit is cut. */
static void tg_locale_put(char *dst, size_t size, size_t *n, const char *s,
                          size_t len)
{
    while (len > 0U && *n + 1U < size) {
        dst[(*n)++] = *s++;
        --len;
    }
}

const char *tg_locale_format(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    size_t n = 0U;

    if (dst == 0 || size == 0U) {
        return dst;
    }
    va_start(ap, fmt);
    while (*fmt != '\0') {
        char spec[16];
        char tmp[48];
        size_t k = 0U;
        int left = 0;
        int width = 0;
        int prec = -1;
        int is_long = 0;
        char conv;

        if (*fmt != '%') {
            tg_locale_put(dst, size, &n, fmt, 1U);
            ++fmt;
            continue;
        }
        spec[k++] = *fmt++;
        while ((*fmt == '-' || *fmt == '0') && k < 4U) {
            if (*fmt == '-') {
                left = 1;
            }
            spec[k++] = *fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 40) {
                width = width * 10 + (*fmt - '0');
            }
            ++fmt;
        }
        if (width > 40) {
            width = 40;
        }
        if (width > 0) {
            if (width >= 10) {
                spec[k++] = (char)('0' + width / 10);
            }
            spec[k++] = (char)('0' + width % 10);
        }
        if (*fmt == '.') {
            ++fmt;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                if (prec < 10000) {
                    prec = prec * 10 + (*fmt - '0');
                }
                ++fmt;
            }
        }
        if (*fmt == 'l') {
            is_long = 1;
            spec[k++] = *fmt++;
        } else if (*fmt == 'h') {
            ++fmt;
        }
        conv = *fmt;
        if (conv == '\0') {
            break;
        }
        ++fmt;
        spec[k++] = conv;
        spec[k] = '\0';
        if (conv == '%') {
            tg_locale_put(dst, size, &n, "%", 1U);
        } else if (conv == 's') {
            const char *str = va_arg(ap, const char *);
            size_t len;
            int pad;

            if (str == 0) {
                str = "";
            }
            len = strlen(str);
            if (prec >= 0 && len > (size_t)prec) {
                len = (size_t)prec;
            }
            pad = width > (int)len ? width - (int)len : 0;
            while (!left && pad-- > 0) {
                tg_locale_put(dst, size, &n, " ", 1U);
            }
            tg_locale_put(dst, size, &n, str, len);
            while (left && pad-- > 0) {
                tg_locale_put(dst, size, &n, " ", 1U);
            }
        } else if (conv == 'c') {
            tmp[0] = (char)va_arg(ap, int);
            tg_locale_put(dst, size, &n, tmp, 1U);
        } else if (conv == 'd' || conv == 'i') {
            if (is_long) {
                sprintf(tmp, spec, va_arg(ap, long));
            } else {
                sprintf(tmp, spec, va_arg(ap, int));
            }
            tg_locale_put(dst, size, &n, tmp, strlen(tmp));
        } else if (conv == 'u' || conv == 'x' || conv == 'X') {
            if (is_long) {
                sprintf(tmp, spec, va_arg(ap, unsigned long));
            } else {
                sprintf(tmp, spec, va_arg(ap, unsigned int));
            }
            tg_locale_put(dst, size, &n, tmp, strlen(tmp));
        } else {
            /* a conversion of no catalog's (scripts/catalog.py refuses
               them): shown as written, nothing taken */
            tg_locale_put(dst, size, &n, spec, k);
        }
    }
    va_end(ap);
    dst[n] = '\0';
    return dst;
}

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
    /* the bounded formatter: as sprintf where it fits, cut where not */
    {
        char a[96];
        char b[96];
        char small[8];

        sprintf(a, "%d %s|%-6s|%5s|%.3s %lu %05d %x %X %ld%% %c %u",
                -42, "ab", "cd", "ef", "ghijk", 4000000000UL, 42, 255U, 255U,
                -7L, 'z', 7U);
        tg_locale_format(b, sizeof(b),
                         "%d %s|%-6s|%5s|%.3s %lu %05d %x %X %ld%% %c %u",
                         -42, "ab", "cd", "ef", "ghijk", 4000000000UL, 42,
                         255U, 255U, -7L, 'z', 7U);
        if (strcmp(a, b) != 0) {
            printf("locale self-test: format gave \"%s\", not \"%s\"\n",
                   b, a);
            return 2;
        }
        tg_locale_format(small, sizeof(small), "%s and %d", "abcdef", 12345);
        if (strcmp(small, "abcdef ") != 0) {
            printf("locale self-test: a cut format gave \"%s\"\n", small);
            return 2;
        }
        {
            /* volatile: the compiler's printf check would refuse a
               visible null, which is just what this tries */
            const char *volatile none = 0;

            tg_locale_format(small, sizeof(small), "%s", none);
        }
        if (small[0] != '\0') {
            puts("locale self-test: a null string was not empty");
            return 2;
        }
    }
    /* Latin-1 to UTF-8: ASCII untouched and not copied, a letter above
       127 as two bytes, a long text cut between letters */
    {
        static const char plain[] = "Photo";
        char longer[TG_LOCALE_UTF8_MAX + 40];
        const char *u;
        size_t i;

        if (tg_locale_utf8(plain) != plain || tg_locale_utf8(0)[0] != '\0') {
            puts("locale self-test: an ASCII text was not passed through");
            return 2;
        }
        u = tg_locale_utf8("perch\xe9 pi\xf9");
        if (strcmp(u, "perch\xc3\xa9 pi\xc3\xb9") != 0) {
            puts("locale self-test: Latin-1 did not become UTF-8");
            return 2;
        }
        for (i = 0U; i + 1U < sizeof(longer); ++i) {
            longer[i] = (char)0xe8;
        }
        longer[i] = '\0';
        u = tg_locale_utf8(longer);
        i = strlen(u);
        if (i >= (size_t)TG_LOCALE_UTF8_MAX || (i % 2U) != 0U ||
            (unsigned char)u[i - 1U] != 0xa8U) {
            puts("locale self-test: a long UTF-8 text was cut in a letter");
            return 2;
        }
    }
    return 0;
}
#endif
