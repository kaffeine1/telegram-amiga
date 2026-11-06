/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_API_DEFAULT_H
#define TG_API_DEFAULT_H

/* The api pair the client uses when data/telegram-api.txt is absent; the file,
   when present, overrides it. Telegram's terms ask developers not to publish
   their credentials, so the pair is not in the repository: it sits in a local
   header kept out of git, include/tg_api_default.local.h, with two lines,
   #define TG_API_DEFAULT_ID "..." and #define TG_API_DEFAULT_HASH "...".
   The release builds have it; a build without it, CI's among them, needs the
   file as every build did before 0.1. */
#if defined(__has_include)
#if __has_include("tg_api_default.local.h")
#include "tg_api_default.local.h"
#endif
#endif

#endif
