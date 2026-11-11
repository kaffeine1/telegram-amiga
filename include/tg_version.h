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

#define TG_VERSION "0.1"
/* Release date for the Amiga $VER tag (dd.mm.yyyy) -- bump WITH the version. */
#define TG_VERSION_DATE "07.11.2026"

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
/* The public Telegram channel where releases are announced, without the
   "@": a post there that says "Amigram 0.3" is what the update notice reads
   (TG_UPDATE_WORD, a space, the version). Empty: this build never looks.
   Both can be given on the compiler's command line, to try the notice
   against a channel that exists. */
#ifndef TG_UPDATE_CHANNEL
#define TG_UPDATE_CHANNEL ""
#endif
#ifndef TG_UPDATE_WORD
#define TG_UPDATE_WORD TG_APP_NAME
#endif
/* What the same terms (2.2) ask every client to tell its users, shown under
   the title where the program introduces itself. */
#define TG_APP_INTRO_NOTE "A Telegram API client, part of the Telegram ecosystem"
/* The client signs in to an existing account and cannot create one yet: a
   first-time user without an account waited for a code that could never be
   used (field report, 2026-10-21). The number screen says it up front. */
#define TG_APP_ACCOUNT_NOTE "No account yet? Create one in the official Telegram app"

#endif
