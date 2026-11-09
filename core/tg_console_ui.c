/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Console UI role colours. See tg_console_ui.h for the model.
 *
 * Control sequences are built on the TG_UI_CSI introducer (tg_console_ui.h):
 * ESC '[' everywhere except AROS, whose console only understands the native
 * single-byte CSI 0x9B and would print ESC '[' literally.
 *
 * Pen assumptions (stock Workbench-family palettes, verified on real
 * hardware): pen 0 = window grey, pen 1 = black, pen 2 = white, pen 3 =
 * blue/accent. The DARK theme paints the chat white-on-black (bg pen 1) with
 * bold/inverse accents; italics are never used because Amiga consoles render
 * them as sheared glyphs that are hard to read on real screens. On ANSI
 * terminals the same sequences map to black bg / white-ish fg, so one scheme
 * serves every target.
 */

#include "tg_console_ui.h"

/* MorphOS: SGR colour output to the console reproducibly froze the whole
   machine (no ping) during the chat-list load on real hardware, while the
   identical flow with --ui-color off ran clean -- bisected live, 2026-06-10.
   Until the console interaction is understood, colours default OFF there
   (and the black-background theme stays off too: pen 1 rendered red on
   Ambient anyway). /color on or --ui-color on re-enable them explicitly for
   the brave. */
/*
 * Platform capability defaults (the compile-time half of the mini-termcap).
 * sgr_safe=0 on MorphOS: SGR colour output reproducibly froze the machine.
 * dark_theme_ok=0 on MorphOS (Ambient pen 1 is red) and AmigaOS 3 (field
 * report: some shell palettes render the background pen red, unreadable).
 * The runtime half (csi_output / answers_size_query) starts UNKNOWN and is
 * refined by the TUI window-size probe.
 */
static tg_console_caps tg_ui_caps = {
    TG_UI_CSI_OUTPUT_UNKNOWN,
    0,
#if defined(__MORPHOS__) || defined(__MORPHOS)
    0, /* sgr_safe */
    0  /* dark_theme_ok */
#elif defined(__amigaos3__)
    1,
    0
#else
    1,
    1
#endif
};

static int tg_ui_color_mode =
#if defined(__MORPHOS__) || defined(__MORPHOS)
    TG_UI_COLOR_OFF;
#else
    TG_UI_COLOR_AUTO;
#endif
/* Theme default mirrors caps.dark_theme_ok (kept in sync by hand: C89
   static initialisers cannot read the struct). */
static int tg_ui_theme =
#if defined(__MORPHOS__) || defined(__MORPHOS) || defined(__amigaos3__)
    TG_UI_THEME_PLAIN;
#else
    TG_UI_THEME_DARK;
#endif
static int tg_ui_interactive = 0;
static int tg_ui_charset = TG_UI_CHARSET_LATIN1;
static int tg_ui_screen_entered = 0;

const tg_console_caps *tg_console_caps_get(void)
{
    return &tg_ui_caps;
}

void tg_console_caps_note_size_query(int answered)
{
    tg_ui_caps.answers_size_query = answered ? 1 : 0;
    tg_ui_caps.csi_output =
        answered ? TG_UI_CSI_OUTPUT_OK : TG_UI_CSI_OUTPUT_DEAF;
}

void tg_console_ui_set_color_mode(int mode)
{
    if (mode == TG_UI_COLOR_OFF || mode == TG_UI_COLOR_ON ||
        mode == TG_UI_COLOR_AUTO) {
        tg_ui_color_mode = mode;
    }
}

int tg_console_ui_color_mode(void)
{
    return tg_ui_color_mode;
}

void tg_console_ui_set_interactive(int interactive)
{
    tg_ui_interactive = interactive ? 1 : 0;
}

int tg_console_ui_color_active(void)
{
    if (tg_ui_color_mode == TG_UI_COLOR_ON) {
        return 1;
    }
    if (tg_ui_color_mode == TG_UI_COLOR_AUTO) {
        return tg_ui_interactive;
    }
    return 0;
}

void tg_console_ui_set_theme(int theme)
{
    if (theme == TG_UI_THEME_DARK || theme == TG_UI_THEME_PLAIN) {
        tg_ui_theme = theme;
    }
}

int tg_console_ui_theme(void)
{
    return tg_ui_theme;
}

/* Every sequence starts from SGR 0 so segments cannot leak attributes into
   each other; the dark variants re-apply the black background each time so
   the row stays uniform. */
static const char *tg_console_ui_role_sequence(int role)
{
    if (tg_ui_theme == TG_UI_THEME_DARK) {
        switch (role) {
        case TG_UI_ROLE_PEER:
            return TG_UI_CSI "0;1;33;41m"; /* bold accent on black */
        case TG_UI_ROLE_OWN:
            return TG_UI_CSI "0;1;32;41m"; /* bold white on black */
        case TG_UI_ROLE_SYSTEM:
            return TG_UI_CSI "0;30;41m"; /* window-grey on black: quiet */
        case TG_UI_ROLE_NOTIFY:
            return TG_UI_CSI "0;7m"; /* inverse block: pops */
        case TG_UI_ROLE_PROMPT:
            return TG_UI_CSI "0;1;32;41m"; /* bold white on black */
        case TG_UI_ROLE_MARKER:
            return TG_UI_CSI "0;1;33;41m"; /* bold accent on black */
        case TG_UI_ROLE_GROUP:
            return TG_UI_CSI "0;30;41m"; /* quiet context prefix */
        default:
            return TG_UI_CSI "0;32;41m"; /* base: white on black */
        }
    }
    switch (role) {
    case TG_UI_ROLE_PEER:
        return TG_UI_CSI "0;1;33m"; /* bold + pen 3 */
    case TG_UI_ROLE_OWN:
        return TG_UI_CSI "0;1m"; /* bold */
    case TG_UI_ROLE_SYSTEM:
        return TG_UI_CSI "0;33m"; /* pen 3, no italics */
    case TG_UI_ROLE_NOTIFY:
        return TG_UI_CSI "0;7m"; /* inverse video */
    case TG_UI_ROLE_PROMPT:
        return TG_UI_CSI "0;1m"; /* bold */
    case TG_UI_ROLE_MARKER:
        return TG_UI_CSI "0;1;33m"; /* bold + pen 3 */
    case TG_UI_ROLE_GROUP:
        return TG_UI_CSI "0;33m"; /* pen 3 context prefix */
    default:
        return TG_UI_CSI "0m";
    }
}

const char *tg_console_ui_role_string(int role)
{
    if (!tg_console_ui_color_active()) {
        return "";
    }
    return tg_console_ui_role_sequence(role);
}

void tg_console_ui_role(FILE *stream, int role)
{
    if (stream == 0 || !tg_console_ui_color_active()) {
        return;
    }
    fputs(tg_console_ui_role_sequence(role), stream);
}

void tg_console_ui_reset(FILE *stream)
{
    tg_console_ui_role(stream, TG_UI_ROLE_RESET);
}

void tg_console_ui_set_charset(int charset)
{
    if (charset == TG_UI_CHARSET_LATIN1 || charset == TG_UI_CHARSET_UTF8) {
        tg_ui_charset = charset;
    }
}

int tg_console_ui_charset(void)
{
    return tg_ui_charset;
}

void tg_console_ui_enter_screen(FILE *stream)
{
    if (stream == 0 || !tg_console_ui_color_active() ||
        tg_ui_theme != TG_UI_THEME_DARK) {
        return;
    }
    /* Base attributes, then erase the whole window so it adopts the black
       background; scrolled-in lines keep it from there on. */
    fputs(tg_console_ui_role_sequence(TG_UI_ROLE_RESET), stream);
    fputs(TG_UI_CSI "H" TG_UI_CSI "J", stream);
    fflush(stream);
    tg_ui_screen_entered = 1;
}

void tg_console_ui_leave_screen(FILE *stream)
{
    if (stream == 0 || !tg_ui_screen_entered) {
        return;
    }
    fputs(TG_UI_CSI "0m\n", stream);
    fflush(stream);
    tg_ui_screen_entered = 0;
}

void tg_console_ui_end_line(FILE *stream)
{
    if (stream == 0) {
        return;
    }
    if (tg_console_ui_color_active() && tg_ui_theme == TG_UI_THEME_DARK) {
        fputs(TG_UI_CSI "K\n", stream);
        return;
    }
    fputc('\n', stream);
}
