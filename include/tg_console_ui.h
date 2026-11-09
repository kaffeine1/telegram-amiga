/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console UI helpers: semantic colour roles rendered as ECMA-48 SGR
 * sequences. Amiga-family consoles (AmigaOS 3.x CON:, MorphOS, AmigaOS 4.x,
 * AROS) and ordinary ANSI terminals share the same escape dialect, so one
 * implementation serves every target; colours map to console pens 0-7 on
 * Amiga screens and to the standard palette elsewhere.
 */

#ifndef TG_CONSOLE_UI_H
#define TG_CONSOLE_UI_H

#include <stdio.h>

/* CSI introducer for console control sequences. The AROS console does not
   translate ESC '[' into CSI (it prints it literally on screen), so AROS
   i386 (ABIv0, the old console) uses the native Amiga single-byte CSI 0x9B.
   The x86_64 lane runs on the modern (deadwood) base whose console is the
   exact OPPOSITE: it no longer recognises lone 0x9B on output (drawn as a
   glyph -- it is an invalid UTF-8 byte there) but interprets 7-bit ESC '['
   fine (verified on the hosted bench: the size probe printed "0 q" as text
   with 0x9B, while ESC[ SGR rendered). So the CSI form is per-AROS-lane;
   input parsing accepts both forms everywhere already. Every other target
   keeps ESC '[', which Amiga console.device descendants accept natively and
   ANSI terminals over ssh/serial require. Adjacent string-literal
   concatenation builds the full sequences at compile time. */
#if defined(__AROS__) && !defined(__x86_64__)
#define TG_UI_CSI "\x9b"
#else
#define TG_UI_CSI "\033["
#endif

/* Semantic output roles. The mapping to concrete SGR attributes lives in one
   place (tg_console_ui.c) so the colour scheme can be tuned per feedback from
   real consoles without touching call sites. */
#define TG_UI_ROLE_RESET 0
#define TG_UI_ROLE_PEER 1    /* other people's names in the transcript */
#define TG_UI_ROLE_OWN 2     /* the logged-in user's own name */
#define TG_UI_ROLE_SYSTEM 3  /* status/info lines ("Loading chats...") */
#define TG_UI_ROLE_NOTIFY 4  /* cross-chat notifications */
#define TG_UI_ROLE_PROMPT 5  /* the chat input prompt */
#define TG_UI_ROLE_MARKER 6  /* send-confirmation marker */
#define TG_UI_ROLE_GROUP 7   /* the "[group]" line prefix */

#define TG_UI_COLOR_OFF 0
#define TG_UI_COLOR_ON 1
#define TG_UI_COLOR_AUTO 2

#define TG_UI_CHARSET_LATIN1 0
#define TG_UI_CHARSET_UTF8 1

/* Themes. DARK (default) paints the chat on a black background (console pen
   1 on Amiga screens) with high-contrast foregrounds and never uses italics
   (Amiga consoles shear the glyphs, which is barely readable). PLAIN leaves
   the window background alone and only colours the foreground. */
#define TG_UI_THEME_DARK 0
#define TG_UI_THEME_PLAIN 1

/*
 * Console capabilities (the mini-termcap). Two days of escape-code
 * whack-a-mole across 4 platforms x N console handlers taught us that the
 * "what can this console actually do" answers must live in ONE place:
 * compile-time platform defaults refined by runtime probes, read by every
 * call site that would otherwise guess.
 *
 * csi_output: does the console interpret our CSI introducer at all? One
 * AROS icon-console type prints 0x9B as a glyph; until the TUI size probe
 * has run the answer is UNKNOWN and writers should assume OK.
 */
#define TG_UI_CSI_OUTPUT_UNKNOWN 0
#define TG_UI_CSI_OUTPUT_OK 1
#define TG_UI_CSI_OUTPUT_DEAF 2

typedef struct tg_console_caps {
    int csi_output;          /* TG_UI_CSI_OUTPUT_*: refined by the TUI probe */
    int answers_size_query;  /* CSI 0 SP q answered (1) or timed out (0) */
    int sgr_safe;            /* SGR colour output known safe (MorphOS: no) */
    int dark_theme_ok;       /* black-background theme is a sane default */
} tg_console_caps;

/* Read-only view of the current capabilities. */
const tg_console_caps *tg_console_caps_get(void);

/* The TUI window-size probe feeds its outcome here: answered==1 marks the
   console CSI-capable; answered==0 with raw stdin active marks it deaf
   (the probe sequence went to the screen instead of being interpreted). */
void tg_console_caps_note_size_query(int answered);

/* Colour mode: ON / OFF / AUTO (default). In AUTO colours activate only once
   the chat marks the session interactive (raw console mode engaged), so
   redirected output and smoke-test logs never contain escape bytes. */
void tg_console_ui_set_color_mode(int mode);
int tg_console_ui_color_mode(void);

/* Interactive hint feeding AUTO mode (set when stdin raw mode succeeds). */
void tg_console_ui_set_interactive(int interactive);

/* 1 when role sequences are currently emitted. */
int tg_console_ui_color_active(void);

/* Emit the SGR sequence for a role (no-op when colours are inactive). */
void tg_console_ui_role(FILE *stream, int role);

/* The same sequence as a string ("" when colours are inactive), for callers
   that assemble whole lines in buffers (the full-screen TUI). */
const char *tg_console_ui_role_string(int role);

/* Return to the theme's base attributes (dark: white on black; plain: SGR
   reset). Use after every role segment so the background stays uniform. */
void tg_console_ui_reset(FILE *stream);

void tg_console_ui_set_theme(int theme);
int tg_console_ui_theme(void);

/* Enter/leave the themed chat screen. In the dark theme (colours active)
   enter paints the whole console window black by setting the base attributes
   and erasing the display; leave restores plain attributes. No-ops in the
   plain theme or when colours are off. */
void tg_console_ui_enter_screen(FILE *stream);
void tg_console_ui_leave_screen(FILE *stream);

/* End an output line. In the dark theme this erases to the end of the line
   first (ESC [ K) so the row is painted with the black background all the
   way to the window border -- consoles that scroll in rows with the plain
   window colour (AmigaOS 3.x) would otherwise show black only behind the
   text. Elsewhere it is just a newline. */
void tg_console_ui_end_line(FILE *stream);

/* Output charset for targets whose display layer transcodes UTF-8 to
   ISO-8859-1 (the Amiga consoles). UTF8 passes message text through raw and
   upgrades markers to real glyphs; pick it when running over ssh or any
   UTF-8 terminal. Targets compiled without the Latin-1 display layer ignore
   this and always behave as UTF8. */
void tg_console_ui_set_charset(int charset);
int tg_console_ui_charset(void);

#endif
