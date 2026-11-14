/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single source of truth for the human-facing version string. It is shown in
 * the GUI About box and the startup banner, and read by
 * scripts/package-human-release.sh (default VERSION), so the binary and the
 * package always agree. Bump this once per release.
 */
#ifndef TG_VERSION_H
#define TG_VERSION_H

#define TG_VERSION "0.5"
/* Release date for the Amiga $VER tag (dd.mm.yyyy) -- bump WITH the version. */
#define TG_VERSION_DATE "14.11.2026"

/* "alpha" while the number has three components, "beta" from 0.1 on.
   THE BETA STARTS AT 0.1, NOT AT 0.1.0. An AmigaOS version cookie is two
   integers, version.revision, so every release so far has read as plain "0.0"
   to the system and to any tool that compares versions, AmiUpdate included.
   From the beta the public number and the cookie are the same two integers:
   0.1, then 0.2, and one day 1.0. Never map 0.0.9x onto 0.9x: the beta would
   then look like a downgrade, revision 1 against revision 93. Bump this word
   with the number; the packaging and the release check read this very word
   from this header, so nothing is left saying alpha by accident. */
#define TG_VERSION_CHANNEL "beta"

/* The program's name: the executable, its icons, the drawer, the archives
   and the version string. Until 0.0.99 it was TelegramAmiga; the files the
   client keeps (telegram-auth.bin and the data drawer) kept their names
   through the rename, so a session moves by copying them. */
#define TG_APP_NAME "Amigram"
/* The name the program shows as its title: window, screen, About, the login
   screen, the text client. Telegram's API terms (2.3) let an app's title
   carry the word Telegram only after "Unofficial"; Amigram needs no
   qualifier. The TUI title is also how the platform code finds its console
   window again, so both come from here. */
#define TG_APP_TITLE TG_APP_NAME
/* The line after the name for the first releases under it, so that whoever
   knew the old name still recognises the program: About, the login screen
   where it fits, the text client's banner. The terms (2.3) allow the word
   Telegram in a title only after "Unofficial". The packaging writes the same
   line in the readmes and manuals (TAGLINE in package-human-release.sh). */
#define TG_APP_TAGLINE "Unofficial Telegram Amiga Client"
#define TG_APP_FULL_TITLE TG_APP_NAME " - " TG_APP_TAGLINE
#define TG_APP_TUI_TITLE TG_APP_TITLE " TUI"
/* The public Telegram channels or groups where releases are announced:
   their usernames, with commas between them ("first,second"; an "@" in
   front is allowed, eight names at most). A post in any of them that says
   "Amigram 0.3" is what the update notice reads (TG_UPDATE_WORD, a space,
   the version); the highest version of the day's round counts. In a
   channel only its administrators post; in a group any member can, so a
   group in this list lets its members raise the notice. Empty: this build
   never looks. Both can be given on the compiler's command line, to try
   the notice against channels that exist. */
#ifndef TG_UPDATE_CHANNELS
#define TG_UPDATE_CHANNELS "amigram1"
#endif
#ifndef TG_UPDATE_WORD
#define TG_UPDATE_WORD TG_APP_NAME
#endif
/* The WEBP datatype this program offers to fetch and install where the
   system has none, so that stickers can be pictures (AmigaOS 3, AmigaOS 4
   and MorphOS; AROS has one of its own). It is not in the archives: it is
   a project of its own (github.com/kaffeine1/webp-datatype) and is fetched
   only when the user asks for it. Here are the public channel that posts
   its files, and for each file the name it is posted under, its size and
   its SHA-256 (64 lowercase hex digits). Only a file with that very hash
   is ever kept: what gets installed is decided here, when a release is
   cut, and not by what the channel comes to hold later. A posted file is
   never replaced: a new build of the datatype is a new version, posted
   under new names and pinned by a new release of this program, so the
   older releases go on finding theirs. An empty hash = no offer, and the
   settings line says there is no datatype. scripts/webp-datatype-pin.sh
   writes the twelve lines below from the files of a release of the
   datatype and puts those files, named as here, where they can be posted
   from. The channel can be given on the compiler's command line: an empty
   name stands for the account's Saved Messages, which is how the offer is
   tried without posting anything where others read. The same channel is
   in TG_UPDATE_CHANNELS: a caption there must never put a number right
   after the word "Amigram" ("Amigram 45.1 ..."), or every client reads
   it as a release of this program. */
#ifndef TG_WEBP_DT_CHANNEL
#define TG_WEBP_DT_CHANNEL "amigram1"
#endif
#define TG_WEBP_DT_OS3_NAME "webp-45.1-os3.datatype"
#define TG_WEBP_DT_OS3_SIZE 130464UL
#define TG_WEBP_DT_OS3_SHA256 "32d20714fec40237b96124de75ebbadd2324f654110163141ebeb3bef52bfbd9"
#define TG_WEBP_DT_OS4_NAME "webp-45.1-os4.datatype"
#define TG_WEBP_DT_OS4_SIZE 223524UL
#define TG_WEBP_DT_OS4_SHA256 "9c487032571de5d2c30535c1201cdf69144612a7ae34faf33b7362ec6f132326"
#define TG_WEBP_DT_MOS_NAME "webp-45.1-morphos.datatype"
#define TG_WEBP_DT_MOS_SIZE 176648UL
#define TG_WEBP_DT_MOS_SHA256 "b2ba6a951fdac2ee3f75a0206fe39e8fbf8937e8cae1d6b608c9facc309e415d"
#define TG_WEBP_DT_DESC_NAME "WebP-45.1.descriptor"
#define TG_WEBP_DT_DESC_SIZE 142UL
#define TG_WEBP_DT_DESC_SHA256 "ba3d7bb91221b090904d65d997f41de74febe14a22d6385a2377a1051ac4202b"
/* the library of the system this build is for */
#if defined(__amigaos4__)
#define TG_WEBP_DT_NAME TG_WEBP_DT_OS4_NAME
#define TG_WEBP_DT_SIZE TG_WEBP_DT_OS4_SIZE
#define TG_WEBP_DT_SHA256 TG_WEBP_DT_OS4_SHA256
#elif defined(__MORPHOS__)
#define TG_WEBP_DT_NAME TG_WEBP_DT_MOS_NAME
#define TG_WEBP_DT_SIZE TG_WEBP_DT_MOS_SIZE
#define TG_WEBP_DT_SHA256 TG_WEBP_DT_MOS_SHA256
#else
#define TG_WEBP_DT_NAME TG_WEBP_DT_OS3_NAME
#define TG_WEBP_DT_SIZE TG_WEBP_DT_OS3_SIZE
#define TG_WEBP_DT_SHA256 TG_WEBP_DT_OS3_SHA256
#endif
/* What the same terms (2.2) ask every client to tell its users, shown under
   the title where the program introduces itself. */
#define TG_APP_INTRO_NOTE "A Telegram API client, part of the Telegram ecosystem"
/* The client signs in to an existing account and cannot create one yet: a
   first-time user without an account waited for a code that could never be
   used (field report, 2026-10-21). The number screen says it up front. */
#define TG_APP_ACCOUNT_NOTE "No account yet? Create one in the official Telegram app"

#endif
