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

#define TG_VERSION "0.0.93"
/* Release date for the Amiga $VER tag (dd.mm.yyyy) -- bump WITH the version. */
#define TG_VERSION_DATE "12.09.2026"

/* "alpha" while the number has three components, "beta" from 0.1 on.
   THE BETA STARTS AT 0.1, NOT AT 0.1.0. An AmigaOS version cookie is two
   integers, version.revision, so every release so far has read as plain "0.0"
   to the system and to any tool that compares versions, AmiUpdate included.
   From the beta the public number and the cookie are the same two integers:
   0.1, then 0.2, and one day 1.0. Never map 0.0.9x onto 0.9x: the beta would
   then look like a downgrade, revision 1 against revision 93. Bump this word
   with the number; the packaging and the release check derive the same word
   from the number itself, so nothing is left saying alpha by accident. */
#define TG_VERSION_CHANNEL "alpha"

#endif
