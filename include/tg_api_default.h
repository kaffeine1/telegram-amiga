/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_API_DEFAULT_H
#define TG_API_DEFAULT_H

/* The api pair the client uses when data/telegram-api.txt is absent or
   unusable; a complete file overrides it. Telegram's terms ask developers not
   to publish their credentials, so from 0.1 the pair is no longer in the
   repository or in the packages (it was, up to 0.0.99, and stays in the
   history of both). It sits in a local header kept out of git,
   include/tg_api_default.local.h, with two lines,
   #define TG_API_DEFAULT_ID "..." and #define TG_API_DEFAULT_HASH "...".
   The release builds have it; a build without it, CI's among them, needs the
   file as every build did before 0.1.
   Should the pair ever be replaced: every package up to 0.0.99 shipped the
   old one as data/telegram-api.txt, and users copy that drawer across, so
   the new build must ignore a file that carries the old api_id. */
#if defined(__has_include)
#if __has_include("tg_api_default.local.h")
#include "tg_api_default.local.h"
#endif
#endif

/* Both halves, or the build behaves as one without a pair. */
#if defined(TG_API_DEFAULT_ID) && defined(TG_API_DEFAULT_HASH)
#define TG_API_HAS_DEFAULT 1
#endif

#endif
