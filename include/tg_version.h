/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Single source of truth for the human-facing version string. It is shown in
 * the GUI About box and the startup banner, and read by
 * scripts/package-human-release.sh (default VERSION), so the binary and the
 * package always agree. Bump this once per release.
 */
#ifndef TG_VERSION_H
#define TG_VERSION_H

#define TG_VERSION "0.0.91"
/* Release date for the Amiga $VER tag (dd.mm.yyyy) -- bump WITH the version. */
#define TG_VERSION_DATE "27.08.2026"

#endif
