/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Full-screen console TUI. See tg_console_tui.h for the model and the
 * console sequences it relies on.
 */

#include <string.h>

#include "tg_console_tui.h"
#include "tg_console_ui.h"
#include "tg_platform.h"

#if defined(TG_DIAG_TRACE)
/* Diagnostic builds trace the window-size handshake: it is the first time
   the chat waits on the console, right after the last marker a 68000 field
   report reached. */
void tg_gui_log(const char *msg);
#define TG_TUI_DIAG(text) tg_gui_log(text)
#else
#define TG_TUI_DIAG(text) ((void)0)
#endif

static int tg_tui_active = 0;
static int tg_tui_enabled = 1;
static int tg_tui_resize_flag = 0;
static unsigned int tg_tui_rows = 0U;
static unsigned int tg_tui_columns = 0U;
static unsigned int tg_tui_composer_rows = 1U;
static int tg_tui_composer_cache_valid = 0;

/* Status is row 1. The composer grows upward from the last row and keeps one
   separator row between itself and the transcript. */
#define TG_TUI_REGION_TOP 2U
#define TG_TUI_MIN_ROWS 8U
#define TG_TUI_MIN_COLUMNS 20U
#define TG_TUI_COMPOSER_MAX_ROWS 3U
#define TG_TUI_WRAP_INDENT 2U

static unsigned int tg_tui_region_rows_for(unsigned int composer_rows)
{
    unsigned int separator;
    unsigned int bottom;

    separator = tg_tui_rows - composer_rows;
    bottom = separator - 1U;
    return bottom >= TG_TUI_REGION_TOP
               ? bottom - TG_TUI_REGION_TOP + 1U
               : 0U;
}

static unsigned int tg_tui_separator_row(void)
{
    return tg_tui_rows - tg_tui_composer_rows;
}

static unsigned int tg_tui_region_bottom(void)
{
    return tg_tui_separator_row() - 1U;
}

static unsigned int tg_tui_region_rows(void)
{
    return tg_tui_region_rows_for(tg_tui_composer_rows);
}

static unsigned int tg_tui_composer_top(void)
{
    return tg_tui_separator_row() + 1U;
}

static void tg_tui_goto(FILE *stream, unsigned int row, unsigned int column)
{
    fprintf(stream, TG_UI_CSI "%u;%uH", row, column);
}

/* Reads one CSI report from the console input, skipping unrelated queued
   reports (e.g. NEWSIZE events, CSI 12;...|) until one ends with `final`.
   Returns 1 with the numeric parameters in values/value_count, 0 on timeout
   or when nothing matching shows up within the guard budget. */
static int tg_tui_read_csi_report(char final, unsigned long values[8],
                                  unsigned long *value_count)
{
    char ch;
    unsigned long current;
    int have_digit;
    int guard;

    for (guard = 0; guard < 16; ++guard) {
        int introducer_tries;
        int byte_tries;

        /* Wait for a CSI introducer (tolerate pending unrelated bytes). */
        introducer_tries = 0;
        for (;;) {
            if (tg_platform_stdin_read_char(2UL, &ch) <= 0) {
                return 0;
            }
            if (ch == (char)0x9b || ch == (char)0x1b) {
                if (ch == (char)0x1b) {
                    if (tg_platform_stdin_read_char(1UL, &ch) <= 0 ||
                        ch != '[') {
                        continue;
                    }
                }
                break;
            }
            if (++introducer_tries > 64) {
                return 0;
            }
        }
        *value_count = 0UL;
        current = 0UL;
        have_digit = 0;
        for (byte_tries = 0; byte_tries < 32; ++byte_tries) {
            if (tg_platform_stdin_read_char(1UL, &ch) <= 0) {
                return 0;
            }
            if (ch >= '0' && ch <= '9') {
                current = (current * 10UL) + (unsigned long)(ch - '0');
                have_digit = 1;
                continue;
            }
            if (have_digit && *value_count < 8UL) {
                values[*value_count] = current;
                ++(*value_count);
            }
            current = 0UL;
            have_digit = 0;
            if (ch == ';' || ch == ' ') {
                continue;
            }
            break; /* final byte */
        }
        if (ch != final) {
            /* Another report (e.g. a queued NEWSIZE event, final '|'):
               ignore it and wait for the real answer. */
            continue;
        }
        return 1;
    }
    return 0;
}

int tg_console_tui_query_size(FILE *stream,
                              unsigned int *rows,
                              unsigned int *columns)
{
    unsigned long values[8];
    unsigned long value_count;

    if (stream == 0 || rows == 0 || columns == 0) {
        return 0;
    }
#if defined(__AROS__)
    /* The AROS console does not interpret CSI on OUTPUT while the handler is
       in RAW mode (verified on the modern x86_64/deadwood base: cooked mode
       executes SGR and cursor moves, raw mode DRAWS them as text; the classic
       i386 console never answered the probe either). The chat needs raw for
       per-key input, so a full-screen layout is not attainable here -- and
       sending the probe would only paint "0 q" garbage into the scrollback.
       Skip it: the caller marks the query unanswered, the mini-termcap keeps
       CSI output off, and the linear flow is the honest presentation. */
    return 0;
#endif
    /* Stage 1 -- Amiga WINDOW STATUS REQUEST: the classic console answers on
       the input stream with CSI 1;1;<rows>;<cols> SP r. Understood by every
       console.device descendant (OS3/OS4/MorphOS, AROS i386). */
    fputs(TG_UI_CSI "0 q", stream);
    fflush(stream);
    TG_TUI_DIAG("diag: tui size query sent (window status request)");
    if (tg_tui_read_csi_report('r', values, &value_count)) {
        TG_TUI_DIAG("diag: tui window status answered");
        if (value_count < 4UL || values[2] < TG_TUI_MIN_ROWS ||
            values[3] < TG_TUI_MIN_COLUMNS || values[2] > 300UL ||
            values[3] > 1000UL) {
            return 0;
        }
        *rows = (unsigned int)values[2];
        *columns = (unsigned int)values[3];
        return 1;
    }
    /* Stage 2 -- ANSI DSR fallback: the modern AROS x86_64 (deadwood) console
       does not implement the Amiga status request (prints it as text) but
       handles core ANSI: park the cursor at the far corner (clamped to the
       window edge) and ask for its position -- the CSI <row>;<col> R report
       IS the window size. No cursor restore needed: the caller either paints
       the full-screen chrome right away or the linear flow scrolls on. */
    TG_TUI_DIAG("diag: tui window status unanswered, trying the DSR");
    fputs(TG_UI_CSI "9999;9999H" TG_UI_CSI "6n", stream);
    fflush(stream);
    if (tg_tui_read_csi_report('R', values, &value_count)) {
        TG_TUI_DIAG("diag: tui DSR answered");
        if (value_count < 2UL || values[0] < TG_TUI_MIN_ROWS ||
            values[1] < TG_TUI_MIN_COLUMNS || values[0] > 300UL ||
            values[1] > 1000UL) {
            return 0;
        }
        *rows = (unsigned int)values[0];
        *columns = (unsigned int)values[1];
        return 1;
    }
    TG_TUI_DIAG("diag: tui size: no answer, linear layout");
    return 0;
}

int tg_console_tui_active(void)
{
    return tg_tui_active;
}

/* Returns the byte after a CSI sequence, or NULL when text does not point at
   one. Both the single-byte Amiga CSI and ESC '[' ANSI form are accepted. */
static const char *tg_tui_after_csi(const char *text)
{
    const char *p;

    if (text == 0) {
        return 0;
    }
    p = text;
    if (*p == (char)0x9b) {
        ++p;
    } else if (*p == (char)0x1b && p[1] == '[') {
        p += 2;
    } else {
        return 0;
    }
    while (*p != '\0' &&
           !((unsigned char)*p >= 0x40U &&
             (unsigned char)*p <= 0x7eU)) {
        ++p;
    }
    if (*p != '\0') {
        ++p;
    }
    return p;
}

static unsigned int tg_tui_visible_between(const char *start,
                                           const char *end)
{
    const char *after;
    unsigned int visible;

    visible = 0U;
    while (start != 0 && start < end && *start != '\0') {
        after = tg_tui_after_csi(start);
        if (after != 0) {
            start = after;
            continue;
        }
        if ((unsigned char)*start >= 0x20U) {
            ++visible;
        }
        ++start;
    }
    return visible;
}

typedef struct tg_tui_wrap_piece {
    const char *line_start;
    const char *start;
    const char *end;
    const char *next;
    unsigned int indent;
} tg_tui_wrap_piece;

typedef struct tg_tui_wrap_iter {
    const char *line;
    const char *cursor;
    unsigned int width;
    int first;
    int done;
} tg_tui_wrap_iter;

static void tg_tui_wrap_init(tg_tui_wrap_iter *iter,
                             const char *text,
                             unsigned int width)
{
    iter->line = text != 0 ? text : "";
    iter->cursor = iter->line;
    iter->width = width > 0U ? width : 1U;
    iter->first = 1;
    iter->done = 0;
}

/* Produces one visual row without allocating. A continuation drops the space
   that caused the wrap and reserves two cells for its visual indent. */
static int tg_tui_wrap_next(tg_tui_wrap_iter *iter,
                            tg_tui_wrap_piece *piece)
{
    const char *p;
    const char *scan;
    const char *after;
    const char *last_space;
    const char *next;
    unsigned int visible;
    unsigned int capacity;

    if (iter == 0 || piece == 0 || iter->done) {
        return 0;
    }
    p = iter->cursor;
    if (!iter->first) {
        while (*p == ' ') {
            ++p;
        }
    }
    piece->line_start = iter->line;
    piece->start = p;
    piece->indent = iter->first ? 0U : TG_TUI_WRAP_INDENT;
    if (piece->indent >= iter->width) {
        piece->indent = iter->width > 1U ? iter->width - 1U : 0U;
    }
    capacity = iter->width - piece->indent;
    if (capacity == 0U) {
        capacity = 1U;
    }
    scan = p;
    visible = 0U;
    last_space = 0;
    while (*scan != '\0') {
        after = tg_tui_after_csi(scan);
        if (after != 0) {
            scan = after;
            continue;
        }
        if ((unsigned char)*scan < 0x20U) {
            ++scan;
            continue;
        }
        if (visible >= capacity) {
            break;
        }
        if (*scan == ' ') {
            last_space = scan;
        }
        ++visible;
        ++scan;
    }
    if (*scan == '\0') {
        piece->end = scan;
        piece->next = scan;
        iter->done = 1;
    } else {
        if (*scan == ' ') {
            piece->end = scan;
            next = scan;
        } else if (last_space != 0 && last_space > p) {
            piece->end = last_space;
            next = last_space;
        } else {
            piece->end = scan;
            next = scan;
        }
        while (*next == ' ') {
            ++next;
        }
        /* A line consisting only of control bytes must still make progress. */
        if (piece->end == p && *next != '\0') {
            after = tg_tui_after_csi(next);
            if (after != 0) {
                next = after;
            } else {
                ++next;
            }
            piece->end = next;
        }
        piece->next = next;
        iter->done = (*next == '\0');
    }
    iter->cursor = piece->next;
    iter->first = 0;
    return 1;
}

static unsigned long tg_tui_wrap_count(const char *text,
                                       unsigned int width)
{
    tg_tui_wrap_iter iter;
    tg_tui_wrap_piece piece;
    unsigned long count;

    count = 0UL;
    tg_tui_wrap_init(&iter, text, width);
    while (tg_tui_wrap_next(&iter, &piece)) {
        ++count;
    }
    return count;
}

/* Writes text clipped to the window width, padding with erase-to-EOL. */
static void tg_tui_clipped_line(FILE *stream, const char *text)
{
    const char *after;
    unsigned int printed;

    printed = 0U;
    while (text != 0 && *text != '\0' && printed + 1U < tg_tui_columns) {
        /* Console escape sequences do not consume columns; pass them through
           without counting (introducer to final byte 0x40-0x7e). */
        after = tg_tui_after_csi(text);
        if (after != 0) {
            while (text < after) {
                fputc(*text, stream);
                ++text;
            }
            continue;
        }
        if ((unsigned char)*text < 0x20U) {
            /* Control bytes inside transcript content (stray BEL, CR...)
               would render as odd glyphs on some consoles: drop them. */
            ++text;
            continue;
        }
        fputc(*text, stream);
        ++text;
        ++printed;
    }
    if (tg_console_ui_color_active() &&
        tg_console_ui_theme() == TG_UI_THEME_DARK) {
        /* Paint the rest of the row with real spaces: consoles without
           back-colour-erase (AmigaOS 3.x) ignore the SGR background on
           CSI K, but spaces are characters and fill their cells. */
        while (printed + 1U < tg_tui_columns) {
            fputc(' ', stream);
            ++printed;
        }
        return;
    }
    fputs(TG_UI_CSI "K", stream);
}

/* Paints one wrapped piece. Continuations replay only the colour-control
   prefix that preceded them, so a repaint that starts halfway through one
   logical line still has the right attributes. */
static void tg_tui_wrapped_piece(FILE *stream,
                                 const tg_tui_wrap_piece *piece)
{
    const char *p;
    const char *after;
    unsigned int printed;
    unsigned int i;

    printed = 0U;
    p = piece->line_start;
    while (p < piece->start) {
        after = tg_tui_after_csi(p);
        if (after != 0 && after <= piece->start) {
            while (p < after) {
                fputc(*p, stream);
                ++p;
            }
        } else {
            ++p;
        }
    }
    for (i = 0U; i < piece->indent && printed + 1U < tg_tui_columns; ++i) {
        fputc(' ', stream);
        ++printed;
    }
    p = piece->start;
    while (p < piece->end && printed + 1U < tg_tui_columns) {
        after = tg_tui_after_csi(p);
        if (after != 0 && after <= piece->end) {
            while (p < after) {
                fputc(*p, stream);
                ++p;
            }
            continue;
        }
        if ((unsigned char)*p >= 0x20U) {
            fputc(*p, stream);
            ++printed;
        }
        ++p;
    }
    if (tg_console_ui_color_active() &&
        tg_console_ui_theme() == TG_UI_THEME_DARK) {
        while (printed + 1U < tg_tui_columns) {
            fputc(' ', stream);
            ++printed;
        }
    } else {
        fputs(TG_UI_CSI "K", stream);
    }
}

void tg_console_tui_status(FILE *stream, const char *status_text)
{
    if (stream == 0 || !tg_tui_active) {
        return;
    }
    tg_tui_composer_cache_valid = 0;
    tg_tui_goto(stream, 1U, 1U);
    tg_console_ui_role(stream, TG_UI_ROLE_NOTIFY);
    tg_tui_clipped_line(stream, status_text != 0 ? status_text : "");
    tg_console_ui_reset(stream);
    fflush(stream);
}

void tg_console_tui_set_enabled(int enabled)
{
    tg_tui_enabled = enabled ? 1 : 0;
}

/*
 * Transcript backlog: the most recent lines, kept so a repaint (resize,
 * /add round-trip) can replay them into the fresh region instead of
 * starting blank. Lines are stored as handed to tg_console_tui_line --
 * colour sequences included, pre-clip, so a wider window after a resize
 * shows more of each line, not less.
 */
#define TG_TUI_BACKLOG_LINES 64U
#ifndef TG_TUI_BACKLOG_WIDTH
#define TG_TUI_BACKLOG_WIDTH 512U
#endif
#define TG_TUI_HIDDEN_ROWS (TG_TUI_COMPOSER_MAX_ROWS - 1U)
#define TG_TUI_SAVED_ROW_WIDTH (TG_TUI_BACKLOG_WIDTH + 16U)

static char tg_tui_backlog[TG_TUI_BACKLOG_LINES][TG_TUI_BACKLOG_WIDTH];
static unsigned long tg_tui_backlog_total = 0UL;
typedef struct tg_tui_webpage_mark {
    long end_offset;
    unsigned long page_hi;
    unsigned long page_lo;
    unsigned long channel_hi;
    unsigned long channel_lo;
} tg_tui_webpage_mark;
static tg_tui_webpage_mark tg_tui_webpages[TG_TUI_BACKLOG_LINES];
static tg_tui_webpage_mark tg_tui_capture_marks[TG_TUI_BACKLOG_LINES];
static unsigned long tg_tui_capture_mark_count = 0UL;
static FILE *tg_tui_mark_capture = 0;
static char tg_tui_hidden_rows[TG_TUI_HIDDEN_ROWS][TG_TUI_SAVED_ROW_WIDTH];
static unsigned int tg_tui_hidden_row_count = 0U;
static char tg_tui_tail_rows[TG_TUI_HIDDEN_ROWS][TG_TUI_SAVED_ROW_WIDTH];
static unsigned int tg_tui_tail_row_count = 0U;

typedef struct tg_tui_repaint_metrics {
    unsigned long full_region_rows;
    unsigned long transition_rows;
    unsigned long composer_repaints;
    unsigned long fast_edits;
    unsigned long slow_edits;
} tg_tui_repaint_metrics;

static tg_tui_repaint_metrics tg_tui_metrics;

static void tg_tui_tail_clear(void)
{
    tg_tui_tail_row_count = 0U;
}

static void tg_tui_tail_push(const char *text)
{
    unsigned int i;

    if (tg_tui_tail_row_count == TG_TUI_HIDDEN_ROWS) {
        for (i = 1U; i < TG_TUI_HIDDEN_ROWS; ++i) {
            memcpy(tg_tui_tail_rows[i - 1U], tg_tui_tail_rows[i],
                   sizeof(tg_tui_tail_rows[0]));
        }
        --tg_tui_tail_row_count;
    }
    strncpy(tg_tui_tail_rows[tg_tui_tail_row_count],
            text != 0 ? text : "", sizeof(tg_tui_tail_rows[0]) - 1U);
    tg_tui_tail_rows[tg_tui_tail_row_count]
                    [sizeof(tg_tui_tail_rows[0]) - 1U] = '\0';
    ++tg_tui_tail_row_count;
}

static int tg_tui_tail_pop(char *out, unsigned long out_size)
{
    if (out == 0 || out_size == 0UL || tg_tui_tail_row_count == 0U) {
        if (out != 0 && out_size > 0UL) {
            out[0] = '\0';
        }
        return 0;
    }
    --tg_tui_tail_row_count;
    strncpy(out, tg_tui_tail_rows[tg_tui_tail_row_count], out_size - 1UL);
    out[out_size - 1UL] = '\0';
    return 1;
}

static void tg_tui_tail_push_piece(const tg_tui_wrap_piece *piece);

static void tg_tui_backlog_record(const char *text)
{
    char *slot = tg_tui_backlog[tg_tui_backlog_total % TG_TUI_BACKLOG_LINES];
    unsigned long n = 0UL;

    memset(&tg_tui_webpages[tg_tui_backlog_total % TG_TUI_BACKLOG_LINES], 0,
           sizeof(*tg_tui_webpages));
    if (text != 0) {
        while (text[n] != '\0' && n + 1UL < TG_TUI_BACKLOG_WIDTH) {
            slot[n] = text[n];
            ++n;
        }
    }
    slot[n] = '\0';
    ++tg_tui_backlog_total;
}

static unsigned int tg_tui_wrap_width(void)
{
    return tg_tui_columns > 1U ? tg_tui_columns - 1U : 1U;
}

/* Scrolls one VIDEO row through the transcript region. */
static void tg_tui_draw_transcript_piece(FILE *stream,
                                         const tg_tui_wrap_piece *piece)
{
    tg_tui_goto(stream, TG_TUI_REGION_TOP, 1U);
    fputs(TG_UI_CSI "M", stream);
    tg_tui_goto(stream, tg_tui_region_bottom(), 1U);
    fputs(TG_UI_CSI "L", stream);
    tg_tui_wrapped_piece(stream, piece);
    tg_tui_tail_push_piece(piece);
}

/* The drawing half of tg_console_tui_line (no recording). One logical line
   may insert several video rows on a narrow console. */
static void tg_tui_draw_transcript_line(FILE *stream, const char *text)
{
    tg_tui_wrap_iter iter;
    tg_tui_wrap_piece piece;

    tg_tui_wrap_init(&iter, text, tg_tui_wrap_width());
    while (tg_tui_wrap_next(&iter, &piece)) {
        tg_tui_draw_transcript_piece(stream, &piece);
    }
}

/* Scrollback: how many lines back from "live" the transcript view sits.
   0 = live; while scrolled, new lines only enter the backlog and the view
   stays anchored to its content. */
static unsigned long tg_tui_view_offset = 0UL;

static unsigned long tg_tui_backlog_available(void)
{
    return tg_tui_backlog_total > TG_TUI_BACKLOG_LINES
               ? (unsigned long)TG_TUI_BACKLOG_LINES
               : tg_tui_backlog_total;
}

static unsigned long tg_tui_view_offset_max_for_region(unsigned int region)
{
    unsigned long avail = tg_tui_backlog_available();
    unsigned long rows;
    unsigned long kept;
    unsigned long index;

    if (region == 0U || avail == 0UL) {
        return 0UL;
    }
    rows = 0UL;
    kept = 0UL;
    index = tg_tui_backlog_total - avail;
    while (kept < avail && rows < (unsigned long)region) {
        rows += tg_tui_wrap_count(
            tg_tui_backlog[(index + kept) % TG_TUI_BACKLOG_LINES],
            tg_tui_wrap_width());
        ++kept;
    }
    return avail > kept ? avail - kept : 0UL;
}

static unsigned long tg_tui_view_offset_max(void)
{
    return tg_tui_view_offset_max_for_region(tg_tui_region_rows());
}

static void tg_tui_saved_row_put(char *out,
                                 unsigned long out_size,
                                 unsigned long *length,
                                 char ch)
{
    if (*length + 1UL < out_size) {
        out[*length] = ch;
        ++(*length);
    }
}

/* Serialises one already-wrapped video row. This is only used for the two
   transcript rows a growing composer temporarily covers. */
static void tg_tui_copy_wrapped_piece(const tg_tui_wrap_piece *piece,
                                      char *out,
                                      unsigned long out_size)
{
    const char *p;
    const char *after;
    unsigned long length;
    unsigned int i;

    if (out == 0 || out_size == 0UL) {
        return;
    }
    length = 0UL;
    p = piece->line_start;
    while (p < piece->start) {
        after = tg_tui_after_csi(p);
        if (after != 0 && after <= piece->start) {
            while (p < after) {
                tg_tui_saved_row_put(out, out_size, &length, *p++);
            }
        } else {
            ++p;
        }
    }
    for (i = 0U; i < piece->indent; ++i) {
        tg_tui_saved_row_put(out, out_size, &length, ' ');
    }
    p = piece->start;
    while (p < piece->end) {
        after = tg_tui_after_csi(p);
        if (after != 0 && after <= piece->end) {
            while (p < after) {
                tg_tui_saved_row_put(out, out_size, &length, *p++);
            }
            continue;
        }
        if ((unsigned char)*p >= 0x20U) {
            tg_tui_saved_row_put(out, out_size, &length, *p);
        }
        ++p;
    }
    out[length] = '\0';
}

static void tg_tui_tail_push_piece(const tg_tui_wrap_piece *piece)
{
    char row[TG_TUI_SAVED_ROW_WIDTH];

    tg_tui_copy_wrapped_piece(piece, row, sizeof(row));
    tg_tui_tail_push(row);
}

/* Copies one visual row from the backlog as if `region` rows were visible.
   It follows the same bottom-up selection as tg_tui_redraw_region but emits
   only the requested row, so composer transitions stay O(1) in screen rows. */
static void tg_tui_copy_virtual_region_row(unsigned int region,
                                           unsigned int wanted_row,
                                           char *out,
                                           unsigned long out_size)
{
    unsigned long avail;
    unsigned long view_offset;
    unsigned long selected;
    unsigned long remaining;
    unsigned long skip_oldest;
    unsigned long back;
    unsigned long i;
    unsigned long row;

    if (out == 0 || out_size == 0UL) {
        return;
    }
    out[0] = '\0';
    if (region == 0U || wanted_row >= region) {
        return;
    }
    avail = tg_tui_backlog_available();
    view_offset = tg_tui_view_offset;
    if (view_offset > tg_tui_view_offset_max_for_region(region)) {
        view_offset = tg_tui_view_offset_max_for_region(region);
    }
    selected = 0UL;
    remaining = (unsigned long)region;
    skip_oldest = 0UL;
    back = view_offset;
    while (back < avail && remaining > 0UL) {
        unsigned long index;
        unsigned long pieces;

        index = tg_tui_backlog_total - 1UL - back;
        pieces = tg_tui_wrap_count(
            tg_tui_backlog[index % TG_TUI_BACKLOG_LINES],
            tg_tui_wrap_width());
        ++selected;
        if (pieces >= remaining) {
            skip_oldest = pieces - remaining;
            remaining = 0UL;
            break;
        }
        remaining -= pieces;
        ++back;
    }
    if ((unsigned long)wanted_row < remaining) {
        return;
    }
    row = remaining;
    for (i = selected; i > 0UL; --i) {
        unsigned long logical_back;
        unsigned long index;
        unsigned long piece_index;
        unsigned long skip;
        tg_tui_wrap_iter iter;
        tg_tui_wrap_piece piece;

        logical_back = view_offset + i - 1UL;
        index = tg_tui_backlog_total - 1UL - logical_back;
        skip = (i == selected) ? skip_oldest : 0UL;
        piece_index = 0UL;
        tg_tui_wrap_init(
            &iter, tg_tui_backlog[index % TG_TUI_BACKLOG_LINES],
            tg_tui_wrap_width());
        while (tg_tui_wrap_next(&iter, &piece)) {
            if (piece_index >= skip) {
                if (row == (unsigned long)wanted_row) {
                    tg_tui_copy_wrapped_piece(&piece, out, out_size);
                    return;
                }
                ++row;
            }
            ++piece_index;
        }
    }
}

static void tg_tui_paint_separator(FILE *stream)
{
    tg_tui_goto(stream, tg_tui_separator_row(), 1U);
    tg_console_ui_role(stream, TG_UI_ROLE_SYSTEM);
    tg_tui_clipped_line(stream,
                        tg_tui_view_offset > 0UL
                            ? "---- older messages -- Shift+Down: back to "
                              "live ----------------------------------"
                            : "----------------------------------------------"
                              "----------------------------------");
    tg_console_ui_reset(stream);
}

static void tg_tui_paint_saved_row(FILE *stream,
                                   unsigned int row,
                                   const char *text)
{
    tg_tui_goto(stream, row, 1U);
    tg_tui_clipped_line(stream, text != 0 ? text : "");
    tg_console_ui_reset(stream);
}

/* Replays one row that was covered by a growing composer through the current
   transcript geometry. New messages can then scroll normally without losing
   the covered tail or requiring a full-region rebuild. */
static void tg_tui_scroll_saved_row(FILE *stream, const char *text)
{
    tg_tui_goto(stream, TG_TUI_REGION_TOP, 1U);
    fputs(TG_UI_CSI "M", stream);
    tg_tui_goto(stream, tg_tui_region_bottom(), 1U);
    fputs(TG_UI_CSI "L", stream);
    tg_tui_paint_saved_row(stream, tg_tui_region_bottom(), text);
    tg_tui_tail_push(text);
}

static void tg_tui_flush_hidden_rows(FILE *stream)
{
    unsigned int i;

    /* The most recently hidden row is the oldest of the covered tail, so the
       LIFO order is also the chronological order required by scrolling. */
    for (i = tg_tui_hidden_row_count; i > 0U; --i) {
        tg_tui_scroll_saved_row(stream, tg_tui_hidden_rows[i - 1U]);
    }
    tg_tui_hidden_row_count = 0U;
}

/* Changes only the rows whose role changes between transcript, separator and
   composer. This keeps a 1/2/3-row threshold crossing independent of screen
   height, which is essential on slow console.device implementations. */
static void tg_tui_transition_composer(FILE *stream,
                                       unsigned int old_rows,
                                       unsigned int new_rows)
{
    unsigned int delta;
    unsigned int i;
    unsigned int old_region;
    unsigned int new_region;
    unsigned int saved_rows;
    unsigned int missing_rows;

    if (old_rows == new_rows) {
        return;
    }
    old_region = tg_tui_region_rows_for(old_rows);
    if (new_rows > old_rows) {
        delta = new_rows - old_rows;
        for (i = 0U; i < delta &&
                         tg_tui_hidden_row_count < TG_TUI_HIDDEN_ROWS;
             ++i) {
            (void)tg_tui_tail_pop(
                tg_tui_hidden_rows[tg_tui_hidden_row_count],
                sizeof(tg_tui_hidden_rows[0]));
            ++tg_tui_hidden_row_count;
        }
        tg_tui_composer_rows = new_rows;
    } else {
        delta = old_rows - new_rows;
        saved_rows = tg_tui_hidden_row_count < delta
                         ? tg_tui_hidden_row_count
                         : delta;
        missing_rows = delta - saved_rows;
        tg_tui_composer_rows = new_rows;
        new_region = tg_tui_region_rows();
        if (missing_rows > 0U) {
            /* Messages or scroll operations may already have consumed part
               of the saved tail. Grow the remaining rows at the top, where
               the older backlog becomes visible, without moving the tail. */
            tg_tui_goto(stream, TG_TUI_REGION_TOP, 1U);
            for (i = 0U; i < missing_rows; ++i) {
                fputs(TG_UI_CSI "L", stream);
            }
            for (i = 0U; i < missing_rows; ++i) {
                char restored[TG_TUI_SAVED_ROW_WIDTH];

                tg_tui_copy_virtual_region_row(new_region, i, restored,
                                               sizeof(restored));
                tg_tui_paint_saved_row(stream, TG_TUI_REGION_TOP + i,
                                       restored);
            }
        }
        for (i = 0U; i < saved_rows; ++i) {
            --tg_tui_hidden_row_count;
            tg_tui_paint_saved_row(
                stream,
                TG_TUI_REGION_TOP + missing_rows + old_region + i,
                tg_tui_hidden_rows[tg_tui_hidden_row_count]);
            tg_tui_tail_push(
                tg_tui_hidden_rows[tg_tui_hidden_row_count]);
        }
    }
    tg_tui_paint_separator(stream);
    ++tg_tui_metrics.transition_rows; /* separator */
    if (new_rows < old_rows) {
        tg_tui_metrics.transition_rows += (unsigned long)delta;
    }
}

/* Redraws the whole transcript region from the backlog at the current view
   offset; the separator doubles as the scrollback indicator. */
static void tg_tui_redraw_region(FILE *stream)
{
    unsigned long avail = tg_tui_backlog_available();
    unsigned long selected;
    unsigned long remaining;
    unsigned long skip_oldest;
    unsigned long back;
    unsigned long i;
    unsigned long r;
    unsigned int region;
    unsigned int row;

    region = tg_tui_region_rows();
    if (region == 0U) {
        return;
    }
    tg_tui_hidden_row_count = 0U;
    tg_tui_tail_clear();
    tg_tui_composer_cache_valid = 0;
    tg_tui_metrics.full_region_rows += (unsigned long)region;
    if (tg_tui_view_offset > tg_tui_view_offset_max()) {
        tg_tui_view_offset = tg_tui_view_offset_max();
    }
    for (r = 0UL; r < (unsigned long)region; ++r) {
        tg_tui_goto(stream, TG_TUI_REGION_TOP + (unsigned int)r, 1U);
        tg_tui_clipped_line(stream, "");
    }

    /* Select logical lines from newest to oldest until their wrapped video
       rows fill the region. Only the oldest selected line may be clipped. */
    selected = 0UL;
    remaining = (unsigned long)region;
    skip_oldest = 0UL;
    back = tg_tui_view_offset;
    while (back < avail && remaining > 0UL) {
        unsigned long index;
        unsigned long pieces;

        index = tg_tui_backlog_total - 1UL - back;
        pieces = tg_tui_wrap_count(
            tg_tui_backlog[index % TG_TUI_BACKLOG_LINES],
            tg_tui_wrap_width());
        ++selected;
        if (pieces >= remaining) {
            skip_oldest = pieces - remaining;
            remaining = 0UL;
            break;
        }
        remaining -= pieces;
        ++back;
    }
    row = TG_TUI_REGION_TOP + (unsigned int)remaining;
    for (i = selected; i > 0UL; --i) {
        unsigned long logical_back;
        unsigned long index;
        unsigned long piece_index;
        unsigned long skip;
        tg_tui_wrap_iter iter;
        tg_tui_wrap_piece piece;

        logical_back = tg_tui_view_offset + i - 1UL;
        index = tg_tui_backlog_total - 1UL - logical_back;
        skip = (i == selected) ? skip_oldest : 0UL;
        piece_index = 0UL;
        tg_tui_wrap_init(
            &iter, tg_tui_backlog[index % TG_TUI_BACKLOG_LINES],
            tg_tui_wrap_width());
        while (tg_tui_wrap_next(&iter, &piece)) {
            if (piece_index >= skip && row <= tg_tui_region_bottom()) {
                tg_tui_goto(stream, row, 1U);
                tg_tui_wrapped_piece(stream, &piece);
                tg_tui_tail_push_piece(&piece);
                ++row;
            }
            ++piece_index;
        }
    }
    tg_tui_paint_separator(stream);
    fflush(stream);
}

void tg_console_tui_scroll(FILE *stream, int direction)
{
    unsigned long step;
    unsigned long max_offset;

    if (stream == 0 || !tg_tui_active || tg_tui_rows < 4U) {
        return;
    }
    tg_tui_composer_cache_valid = 0;
    /* Scroll by logical messages. Their wrapped video-row count is resolved
       afresh at paint time, so a resize reflows the same history. */
    step = (unsigned long)tg_tui_region_rows() / 2UL;
    if (step == 0UL) {
        step = 1UL;
    }
    max_offset = tg_tui_view_offset_max();
    if (direction > 0) {
        tg_tui_view_offset += step;
        if (tg_tui_view_offset > max_offset) {
            tg_tui_view_offset = max_offset;
        }
    } else {
        tg_tui_view_offset =
            tg_tui_view_offset > step ? tg_tui_view_offset - step : 0UL;
    }
    tg_tui_redraw_region(stream);
}

static void tg_tui_paint_chrome(FILE *stream, const char *status_text)
{
    unsigned int row;

    /* A chrome repaint (enter, resize, /add round-trip) returns to live. */
    tg_tui_view_offset = 0UL;
    /* Base attributes + full clear, then the fixed chrome. */
    tg_console_ui_reset(stream);
    fputs(TG_UI_CSI "H" TG_UI_CSI "J", stream);
    tg_console_tui_status(stream, status_text);
    /* What was on screen, back on screen: the repaint should cost the user
       nothing but a blink. */
    tg_tui_redraw_region(stream);
    for (row = tg_tui_composer_top(); row <= tg_tui_rows; ++row) {
        tg_tui_goto(stream, row, 1U);
        fputs(TG_UI_CSI "K", stream);
    }
    fflush(stream);
}

int tg_console_tui_enter(FILE *stream, const char *status_text)
{
    unsigned int rows;
    unsigned int columns;

    if (stream == 0 || tg_tui_active) {
        return tg_tui_active;
    }
    /* The size probe runs even with the full-screen layout disabled: its
       outcome feeds the mini-termcap (an unanswered probe on a raw console
       marks it CSI-deaf, so other writers stop sending sequences it would
       draw as glyphs). */
    if (!tg_console_tui_query_size(stream, &rows, &columns)) {
        tg_console_caps_note_size_query(0);
        return 0;
    }
    tg_console_caps_note_size_query(1);
    if (!tg_tui_enabled) {
        return 0;
    }
    tg_tui_rows = rows;
    tg_tui_columns = columns;
    tg_tui_composer_rows = 1U;
    tg_tui_composer_cache_valid = 0;
    tg_tui_active = 1;
    tg_tui_resize_flag = 0;
    memset(tg_tui_webpages, 0, sizeof(tg_tui_webpages));
    /* Subscribe to the console's NEWSIZE (12) and CLOSEWINDOW (11) raw
       events: resizes arrive on stdin as CSI 12;...| reports (turned into a
       pending-resize flag) and a close-gadget click as CSI 11;...| (turned
       into a clean quit by the line editor). */
    /* One class per sequence: the original ROM 3.1 console handler mishandles
       the combined "11;12{" subscription (field report 2026-08-05: close
       gadget dead on a plain 3.1 68000). Single-class sequences register
       fine on every console flavour, so send one per class. */
    fputs(TG_UI_CSI "11{", stream);
    fputs(TG_UI_CSI "12{", stream);
    tg_tui_paint_chrome(stream, status_text);
    return 1;
}

void tg_console_tui_note_resize(void)
{
    if (tg_tui_active) {
        tg_tui_resize_flag = 1;
    }
}

int tg_console_tui_resize_pending(void)
{
    return tg_tui_resize_flag;
}

int tg_console_tui_resize(FILE *stream, const char *status_text)
{
    unsigned int rows;
    unsigned int columns;
    char drain;
    int drained;

    tg_tui_resize_flag = 0;
    if (stream == 0 || !tg_tui_active) {
        return 0;
    }
    /* A window drag floods stdin with queued NEWSIZE reports; flush them
       before asking for the final geometry. */
    for (drained = 0; drained < 512; ++drained) {
        if (tg_platform_stdin_read_char(0UL, &drain) <= 0) {
            break;
        }
    }
    if (!tg_console_tui_query_size(stream, &rows, &columns)) {
        /* Keep the old geometry rather than tearing the layout down. */
        return 0;
    }
    if (rows == tg_tui_rows && columns == tg_tui_columns) {
        /* Same geometry (a duplicate end-of-drag report, or a console that
           confirms the subscription with an immediate report): repainting
           would wipe the transcript for nothing. */
        return 0;
    }
    tg_tui_rows = rows;
    tg_tui_columns = columns;
    tg_tui_paint_chrome(stream, status_text);
    return 1;
}

void tg_console_tui_line(FILE *stream, const char *text)
{
    if (stream == 0 || !tg_tui_active) {
        return;
    }
    tg_tui_composer_cache_valid = 0;
    /* Scroll the transcript region: drop its top row (everything below
       shifts up, input row included), then re-open a blank row just above
       the separator so the chrome returns to its place. The line is also
       recorded so resize/re-enter repaints can replay it. */
    tg_tui_backlog_record(text);
    if (tg_tui_view_offset > 0UL) {
        /* Scrolled back: the new line enters the backlog only and the view
           stays anchored to its content (the offset grows with the ring). */
        unsigned long max_offset = tg_tui_view_offset_max();
        if (tg_tui_view_offset < max_offset) {
            ++tg_tui_view_offset;
        }
        return;
    }
    if (tg_tui_hidden_row_count > 0U) {
        tg_tui_flush_hidden_rows(stream);
    }
    tg_tui_draw_transcript_line(stream, text);
    fflush(stream);
}

/* The prompt and a whole message: the composer shows three rows of it
   around the caret, but lays out the text it has in full. */
#define TG_TUI_INPUT_TEXT_MAX (TG_CONSOLE_TUI_MESSAGE_MAX + 160U)

typedef struct tg_tui_composer_plan {
    unsigned long total_pieces;
    unsigned long first_piece;
    unsigned long caret_piece;
    unsigned int rows;
    unsigned int caret_column;
} tg_tui_composer_plan;

static unsigned long tg_tui_build_input_text(
    char out[TG_TUI_INPUT_TEXT_MAX],
    const char *prompt,
    const char *pending,
    unsigned long pending_length,
    unsigned long pending_caret,
    unsigned long *caret_offset)
{
    unsigned long length;
    unsigned long i;

    length = 0UL;
    if (prompt != 0) {
        while (*prompt != '\0' && length + 1UL < TG_TUI_INPUT_TEXT_MAX) {
            out[length++] = *prompt++;
        }
    }
    if (pending_caret > pending_length) {
        pending_caret = pending_length;
    }
    *caret_offset = length + pending_caret;
    if (pending != 0) {
        for (i = 0UL; i < pending_length &&
                          length + 1UL < TG_TUI_INPUT_TEXT_MAX;
             ++i) {
            /* a line break of a pasted text shows as a pilcrow, one cell
               like any letter, so the rows and the caret stay in place */
            out[length++] = pending[i] == '\n' ? (char)0xB6 : pending[i];
        }
    }
    out[length] = '\0';
    if (*caret_offset > length) {
        *caret_offset = length;
    }
    return length;
}

/* Pure composer layout: used by both the painter and the host golden test. */
static void tg_tui_make_composer_plan(const char *text,
                                      unsigned long caret_offset,
                                      unsigned int width,
                                      tg_tui_composer_plan *plan)
{
    const char *caret;
    const char *text_end;
    tg_tui_wrap_iter iter;
    tg_tui_wrap_piece piece;
    unsigned long index;
    int caret_found;

    memset(plan, 0, sizeof(*plan));
    text_end = text + strlen(text);
    if (caret_offset > (unsigned long)(text_end - text)) {
        caret_offset = (unsigned long)(text_end - text);
    }
    caret = text + caret_offset;
    caret_found = 0;
    index = 0UL;
    tg_tui_wrap_init(&iter, text, width);
    while (tg_tui_wrap_next(&iter, &piece)) {
        if (!caret_found && caret >= piece.start && caret < piece.end) {
            plan->caret_piece = index;
            plan->caret_column = piece.indent +
                tg_tui_visible_between(piece.start, caret) + 1U;
            caret_found = 1;
        } else if (!caret_found && !iter.done && caret >= piece.end &&
                   caret < piece.next) {
            plan->caret_piece = index + 1UL;
            plan->caret_column = TG_TUI_WRAP_INDENT + 1U;
            caret_found = 1;
        } else if (!caret_found && iter.done && caret >= piece.end) {
            plan->caret_piece = index;
            plan->caret_column = piece.indent +
                tg_tui_visible_between(piece.start, piece.end) + 1U;
            caret_found = 1;
        }
        ++index;
    }
    plan->total_pieces = index > 0UL ? index : 1UL;
    if (!caret_found) {
        plan->caret_piece = plan->total_pieces - 1UL;
        plan->caret_column = 1U;
    }
    plan->rows = plan->total_pieces > TG_TUI_COMPOSER_MAX_ROWS
                     ? TG_TUI_COMPOSER_MAX_ROWS
                     : (unsigned int)plan->total_pieces;
    if (plan->total_pieces > (unsigned long)plan->rows) {
        if (plan->caret_piece + 1UL > (unsigned long)plan->rows) {
            plan->first_piece =
                plan->caret_piece + 1UL - (unsigned long)plan->rows;
        }
        if (plan->first_piece + (unsigned long)plan->rows >
            plan->total_pieces) {
            plan->first_piece =
                plan->total_pieces - (unsigned long)plan->rows;
        }
    }
    if (plan->caret_piece < plan->first_piece) {
        plan->caret_piece = plan->first_piece;
    } else if (plan->caret_piece >=
               plan->first_piece + (unsigned long)plan->rows) {
        plan->caret_piece =
            plan->first_piece + (unsigned long)plan->rows - 1UL;
    }
    if (plan->caret_column > width + 1U) {
        plan->caret_column = width + 1U;
    }
}

typedef struct tg_tui_composer_layout {
    tg_tui_composer_plan plan;
    unsigned long visible_start[TG_TUI_COMPOSER_MAX_ROWS];
    unsigned int visible_count;
} tg_tui_composer_layout;

#define TG_TUI_CACHED_PROMPT_MAX 96U

typedef struct tg_tui_composer_cache {
    tg_tui_composer_layout layout;
    char prompt[TG_TUI_CACHED_PROMPT_MAX];
    unsigned long pending_length;
    unsigned long pending_caret;
} tg_tui_composer_cache;

static tg_tui_composer_cache tg_tui_composer_cache_state;

static void tg_tui_make_composer_layout(const char *text,
                                        unsigned long caret_offset,
                                        unsigned int width,
                                        tg_tui_composer_layout *layout)
{
    tg_tui_wrap_iter iter;
    tg_tui_wrap_piece piece;
    unsigned long piece_index;

    memset(layout, 0, sizeof(*layout));
    tg_tui_make_composer_plan(text, caret_offset, width, &layout->plan);
    piece_index = 0UL;
    tg_tui_wrap_init(&iter, text, width);
    while (tg_tui_wrap_next(&iter, &piece)) {
        if (piece_index >= layout->plan.first_piece &&
            piece_index < layout->plan.first_piece +
                              (unsigned long)layout->plan.rows &&
            layout->visible_count < TG_TUI_COMPOSER_MAX_ROWS) {
            layout->visible_start[layout->visible_count] =
                (unsigned long)(piece.start - text);
            ++layout->visible_count;
        }
        ++piece_index;
    }
}

static void tg_tui_store_composer_cache(
    const char *prompt,
    unsigned long pending_length,
    unsigned long pending_caret,
    const tg_tui_composer_layout *layout)
{
    const char *safe_prompt;

    safe_prompt = prompt != 0 ? prompt : "";
    strncpy(tg_tui_composer_cache_state.prompt, safe_prompt,
            sizeof(tg_tui_composer_cache_state.prompt) - 1U);
    tg_tui_composer_cache_state
        .prompt[sizeof(tg_tui_composer_cache_state.prompt) - 1U] = '\0';
    tg_tui_composer_cache_state.pending_length = pending_length;
    tg_tui_composer_cache_state.pending_caret = pending_caret;
    tg_tui_composer_cache_state.layout = *layout;
    tg_tui_composer_cache_valid = 1;
}

static int tg_tui_cached_prompt_matches(const char *prompt)
{
    return strcmp(tg_tui_composer_cache_state.prompt,
                  prompt != 0 ? prompt : "") == 0;
}

/* Fast caret-at-end echo on any composer row. The cached layout proves that
   the hardware cursor is already at the end; only the boundary character
   that creates a new visual row falls back to the bounded repaint. */
int tg_console_tui_input_append(FILE *stream,
                                const char *prompt,
                                const char *pending,
                                unsigned long pending_length,
                                char ch)
{
    tg_tui_composer_plan *plan;

    if (stream == 0 || !tg_tui_active || pending == 0 ||
        pending_length == 0UL || (unsigned char)ch < 0x20U ||
        !tg_tui_composer_cache_valid ||
        !tg_tui_cached_prompt_matches(prompt) ||
        tg_tui_composer_cache_state.pending_caret !=
            tg_tui_composer_cache_state.pending_length ||
        pending_length !=
            tg_tui_composer_cache_state.pending_length + 1UL ||
        pending[pending_length - 1UL] != ch) {
        if (stream != 0 && tg_tui_active) {
            ++tg_tui_metrics.slow_edits;
        }
        return 0;
    }
    plan = &tg_tui_composer_cache_state.layout.plan;
    if (plan->rows != tg_tui_composer_rows ||
        plan->caret_column > tg_tui_wrap_width()) {
        ++tg_tui_metrics.slow_edits;
        return 0;
    }
    fputc(ch, stream);
    fflush(stream);
    tg_tui_composer_cache_state.pending_length = pending_length;
    tg_tui_composer_cache_state.pending_caret = pending_length;
    ++plan->caret_column;
    ++tg_tui_metrics.fast_edits;
    return 1;
}

/* A rubout remains O(1) while the wrapped piece starts and viewport stay put.
   Removing the boundary character may pull a word onto the preceding row, so
   that case is detected from fresh geometry and uses the transition painter. */
int tg_console_tui_input_backspace(FILE *stream,
                                   const char *prompt,
                                   const char *pending,
                                   unsigned long pending_length)
{
    static char text[TG_TUI_INPUT_TEXT_MAX]; /* 4 KB: off the stack */
    unsigned long caret_offset;
    tg_tui_composer_layout next;
    tg_tui_composer_plan *old_plan;
    unsigned int i;

    if (stream == 0 || !tg_tui_active ||
        !tg_tui_composer_cache_valid ||
        !tg_tui_cached_prompt_matches(prompt) ||
        tg_tui_composer_cache_state.pending_caret !=
            tg_tui_composer_cache_state.pending_length ||
        tg_tui_composer_cache_state.pending_length != pending_length + 1UL) {
        if (stream != 0 && tg_tui_active) {
            ++tg_tui_metrics.slow_edits;
        }
        return 0;
    }
    (void)tg_tui_build_input_text(text, prompt, pending, pending_length,
                                  pending_length, &caret_offset);
    tg_tui_make_composer_layout(text, caret_offset, tg_tui_wrap_width(),
                                &next);
    old_plan = &tg_tui_composer_cache_state.layout.plan;
    if (next.plan.rows != old_plan->rows ||
        next.plan.total_pieces != old_plan->total_pieces ||
        next.plan.first_piece != old_plan->first_piece ||
        next.plan.caret_piece != old_plan->caret_piece ||
        next.visible_count !=
            tg_tui_composer_cache_state.layout.visible_count ||
        old_plan->caret_column != next.plan.caret_column + 1U) {
        ++tg_tui_metrics.slow_edits;
        return 0;
    }
    for (i = 0U; i < next.visible_count; ++i) {
        if (next.visible_start[i] !=
            tg_tui_composer_cache_state.layout.visible_start[i]) {
            ++tg_tui_metrics.slow_edits;
            return 0;
        }
    }
    fputs("\b \b", stream);
    fflush(stream);
    tg_tui_store_composer_cache(prompt, pending_length, pending_length,
                                &next);
    ++tg_tui_metrics.fast_edits;
    return 1;
}

void tg_console_tui_input_caret(FILE *stream,
                                const char *prompt,
                                const char *pending,
                                unsigned long pending_length,
                                unsigned long pending_caret)
{
    static char text[TG_TUI_INPUT_TEXT_MAX]; /* 4 KB: off the stack */
    unsigned long caret_offset;
    unsigned long piece_index;
    unsigned int row;
    unsigned int old_rows;
    tg_tui_composer_layout layout;
    tg_tui_composer_plan *plan;
    tg_tui_wrap_iter iter;
    tg_tui_wrap_piece piece;

    if (stream == 0 || !tg_tui_active) {
        return;
    }
    (void)tg_tui_build_input_text(text, prompt, pending, pending_length,
                                  pending_caret, &caret_offset);
    tg_tui_make_composer_layout(text, caret_offset, tg_tui_wrap_width(),
                                &layout);
    plan = &layout.plan;
    old_rows = tg_tui_composer_rows;
    if (old_rows != plan->rows) {
        tg_tui_transition_composer(stream, old_rows, plan->rows);
    } else {
        tg_tui_composer_rows = plan->rows;
    }
    for (row = tg_tui_composer_top(); row <= tg_tui_rows; ++row) {
        tg_tui_goto(stream, row, 1U);
        fputs(TG_UI_CSI "K", stream);
    }
    tg_tui_metrics.composer_repaints += (unsigned long)plan->rows;
    row = tg_tui_composer_top();
    piece_index = 0UL;
    tg_tui_wrap_init(&iter, text, tg_tui_wrap_width());
    while (tg_tui_wrap_next(&iter, &piece)) {
        if (piece_index >= plan->first_piece &&
            piece_index < plan->first_piece + (unsigned long)plan->rows) {
            tg_tui_goto(stream, row, 1U);
            tg_tui_wrapped_piece(stream, &piece);
            ++row;
        }
        ++piece_index;
    }
    row = tg_tui_composer_top() +
          (unsigned int)(plan->caret_piece - plan->first_piece);
    tg_tui_goto(stream, row, plan->caret_column);
    fflush(stream);
    tg_tui_store_composer_cache(prompt, pending_length, pending_caret,
                                &layout);
}

void tg_console_tui_input(FILE *stream,
                          const char *prompt,
                          const char *pending,
                          unsigned long pending_length)
{
    tg_console_tui_input_caret(stream, prompt, pending, pending_length,
                               pending_length);
}

void tg_console_tui_leave(FILE *stream)
{
    if (stream == 0 || !tg_tui_active) {
        return;
    }
    tg_tui_active = 0;
    tg_tui_resize_flag = 0;
    tg_tui_composer_rows = 1U;
    tg_tui_composer_cache_valid = 0;
    tg_tui_hidden_row_count = 0U;
    tg_tui_tail_clear();
    fputs(TG_UI_CSI "11}", stream); /* unsubscribe CLOSE raw events */
    fputs(TG_UI_CSI "12}", stream); /* unsubscribe NEWSIZE raw events */
    tg_tui_goto(stream, tg_tui_rows, 1U);
    fputs(TG_UI_CSI "0m\n", stream);
    fflush(stream);
}

static char tg_tui_prompt_text[96];

void tg_console_tui_set_prompt(const char *prompt)
{
    unsigned long i;

    tg_tui_prompt_text[0] = '\0';
    if (prompt == 0) {
        return;
    }
    i = 0UL;
    while (prompt[i] != '\0' && i + 1UL < sizeof(tg_tui_prompt_text)) {
        tg_tui_prompt_text[i] = prompt[i];
        ++i;
    }
    tg_tui_prompt_text[i] = '\0';
}

const char *tg_console_tui_prompt(void)
{
    return tg_tui_prompt_text;
}

FILE *tg_console_tui_capture_begin(FILE *fallback)
{
    FILE *capture;

    if (!tg_tui_active) {
        return fallback;
    }
    capture = tmpfile();
    tg_tui_mark_capture = capture;
    tg_tui_capture_mark_count = 0UL;
    return capture != 0 ? capture : fallback;
}

void tg_console_tui_capture_webpage(FILE *capture,
                                    unsigned long page_hi, unsigned long page_lo,
                                    unsigned long channel_hi, unsigned long channel_lo)
{
    tg_tui_webpage_mark *mark;
    long offset;

    if (!tg_tui_active || capture == 0 || capture != tg_tui_mark_capture ||
        (page_hi == 0UL && page_lo == 0UL) ||
        tg_tui_capture_mark_count == TG_TUI_BACKLOG_LINES) {
        return;
    }
    offset = ftell(capture);
    if (offset < 0L) return;
    mark = &tg_tui_capture_marks[tg_tui_capture_mark_count++];
    mark->end_offset = offset;
    mark->page_hi = page_hi;
    mark->page_lo = page_lo;
    mark->channel_hi = channel_hi;
    mark->channel_lo = channel_lo;
}

/* Insert a preview paragraph in the bounded backlog, preserving the metadata
   on every shifted row. A full ring drops its oldest row as usual. */
static void tg_tui_insert_preview(unsigned long at, const char *text,
                                   unsigned long length)
{
    unsigned long k;
    unsigned long slot;

    for (k = tg_tui_backlog_total; k > at; --k) {
        memcpy(tg_tui_backlog[k % TG_TUI_BACKLOG_LINES],
               tg_tui_backlog[(k - 1UL) % TG_TUI_BACKLOG_LINES],
               TG_TUI_BACKLOG_WIDTH);
        tg_tui_webpages[k % TG_TUI_BACKLOG_LINES] =
            tg_tui_webpages[(k - 1UL) % TG_TUI_BACKLOG_LINES];
    }
    slot = at % TG_TUI_BACKLOG_LINES;
    if (length >= TG_TUI_BACKLOG_WIDTH) length = TG_TUI_BACKLOG_WIDTH - 1UL;
    memcpy(tg_tui_backlog[slot], text, length);
    tg_tui_backlog[slot][length] = '\0';
    memset(&tg_tui_webpages[slot], 0, sizeof(*tg_tui_webpages));
    ++tg_tui_backlog_total;
}

int tg_console_tui_complete_webpage(FILE *stream,
                                   unsigned long page_hi, unsigned long page_lo,
                                   unsigned long channel_hi, unsigned long channel_lo,
                                   const char *text)
{
    unsigned long back;
    unsigned long at;
    unsigned long available;
    int changed = 0;

    if (!tg_tui_active || stream == 0 || text == 0 ||
        (page_hi == 0UL && page_lo == 0UL)) return 0;
    available = tg_tui_backlog_available();
    at = tg_tui_backlog_total;
    for (back = 0UL; back < available; ++back) {
        tg_tui_webpage_mark *mark;
        const char *p;
        unsigned long insert;

        --at;
        if (tg_tui_backlog_total - at > TG_TUI_BACKLOG_LINES) break;
        mark = &tg_tui_webpages[at % TG_TUI_BACKLOG_LINES];
        if (mark->page_hi != page_hi || mark->page_lo != page_lo ||
            mark->channel_hi != channel_hi || mark->channel_lo != channel_lo) continue;
        memset(mark, 0, sizeof(*mark));
        p = text;
        insert = at + 1UL;
        while (*p != '\0') {
            const char *end = strchr(p, '\n');
            unsigned long n = end != 0 ? (unsigned long)(end - p)
                                       : (unsigned long)strlen(p);
            tg_tui_insert_preview(insert++, p, n);
            if (end == 0) break;
            p = end + 1;
        }
        changed = 1;
    }
    if (changed) {
        tg_tui_redraw_region(stream);
        fflush(stream);
    }
    return changed;
}

/* Staging buffers that feed the transcript used to DROP everything past
   their capacity, so a long message arrived cut (field report 2026-08-06,
   a multi-paragraph test message on a 68000). Flush the full buffer as its
   own transcript line instead -- breaking at a word boundary when one is
   near the end -- and carry the remainder over with the wrap indent, so the
   message keeps reading as one paragraph. Returns the new length. */
#define TG_TUI_SPLIT_LOOKBACK 64UL

unsigned long tg_console_tui_line_push(FILE *stream, char *line,
                                       unsigned long capacity,
                                       unsigned long length, char ch)
{
    unsigned long cut;
    unsigned long moved;
    unsigned long i;
    char saved;

    if (line == 0 ||
        capacity < TG_TUI_SPLIT_LOOKBACK + TG_TUI_WRAP_INDENT + 4UL) {
        return length; /* pathological capacity: behave as before */
    }
    if (length + 1UL < capacity) {
        line[length] = ch;
        return length + 1UL;
    }
    cut = length;
    for (i = 0UL; i < TG_TUI_SPLIT_LOOKBACK && i + 1UL < length; ++i) {
        if (line[length - 1UL - i] == ' ') {
            cut = length - 1UL - i;
            break;
        }
    }
    saved = line[cut];
    line[cut] = '\0';
    tg_console_tui_line(stream, line);
    line[cut] = saved;
    if (line[cut] == ' ') {
        ++cut; /* the break consumed that space */
    }
    moved = length - cut;
    memmove(line + TG_TUI_WRAP_INDENT, line + cut, (size_t)moved);
    for (i = 0UL; i < TG_TUI_WRAP_INDENT; ++i) {
        line[i] = ' ';
    }
    length = moved + TG_TUI_WRAP_INDENT;
    if (length + 1UL < capacity) {
        line[length] = ch;
        ++length;
    }
    return length;
}

void tg_console_tui_capture_end(FILE *capture, FILE *fallback)
{
    char line[512];
    unsigned long length;
    int ch;

    if (capture == 0 || capture == fallback || !tg_tui_active) {
        if (capture != 0 && capture != fallback) {
            fclose(capture);
        }
        return;
    }
    rewind(capture);
    length = 0UL;
    for (;;) {
        ch = fgetc(capture);
        if (ch == EOF || ch == '\n') {
            line[length] = '\0';
            if (length > 0UL || ch == '\n') {
                unsigned long mi;
                long end_offset = ftell(capture);

                tg_console_tui_line(fallback, line);
                if (capture == tg_tui_mark_capture) {
                    for (mi = 0UL; mi < tg_tui_capture_mark_count; ++mi) {
                        if (tg_tui_capture_marks[mi].end_offset == end_offset) {
                            tg_tui_webpages[(tg_tui_backlog_total - 1UL) %
                                           TG_TUI_BACKLOG_LINES] = tg_tui_capture_marks[mi];
                        }
                    }
                }
            }
            length = 0UL;
            if (ch == EOF) {
                break;
            }
            continue;
        }
        length = tg_console_tui_line_push(fallback, line, sizeof(line),
                                          length, (char)ch);
    }
    if (capture == tg_tui_mark_capture) {
        tg_tui_mark_capture = 0;
        tg_tui_capture_mark_count = 0UL;
    }
    fclose(capture);
}

#if !defined(TG_NO_SELFTEST)
static void tg_tui_collect_wrap(const char *text,
                                unsigned int columns,
                                char *out,
                                unsigned long out_size)
{
    tg_tui_wrap_iter iter;
    tg_tui_wrap_piece piece;
    const char *p;
    const char *after;
    unsigned long length;
    unsigned int i;
    int first;

    length = 0UL;
    first = 1;
    if (out_size == 0UL) {
        return;
    }
    tg_tui_wrap_init(&iter, text, columns > 1U ? columns - 1U : 1U);
    while (tg_tui_wrap_next(&iter, &piece)) {
        if (!first && length + 1UL < out_size) {
            out[length++] = '|';
        }
        first = 0;
        for (i = 0U; i < piece.indent && length + 1UL < out_size; ++i) {
            out[length++] = ' ';
        }
        p = piece.start;
        while (p < piece.end && length + 1UL < out_size) {
            after = tg_tui_after_csi(p);
            if (after != 0 && after <= piece.end) {
                p = after;
                continue;
            }
            if ((unsigned char)*p >= 0x20U) {
                out[length++] = *p;
            }
            ++p;
        }
    }
    out[length] = '\0';
}

static int tg_tui_expect_wrap(const char *label,
                              const char *text,
                              unsigned int columns,
                              const char *expected)
{
    char actual[640];

    tg_tui_collect_wrap(text, columns, actual, sizeof(actual));
    if (strcmp(actual, expected) != 0) {
        printf("tui layout self-test: %s mismatch\nexpected: %s\nactual:   %s\n",
               label, expected, actual);
        return 0;
    }
    return 1;
}

static int tg_tui_composer_incremental_self_test(void)
{
    FILE *stream;
    const char *stream_path;
    char pending[64];
    unsigned long length;
    unsigned long transitions;
    unsigned int old_rows;
    unsigned int saved_rows;
    unsigned int saved_columns;
    unsigned int saved_composer_rows;
    unsigned int saved_hidden_count;
    unsigned int saved_tail_count;
    unsigned int final_composer_rows;
    int saved_active;
    int saved_cache_valid;
    int tail_ok;
    int ok;
    tg_tui_repaint_metrics saved_metrics;
    tg_tui_repaint_metrics test_metrics;
    tg_tui_composer_cache saved_cache;
    char saved_hidden[TG_TUI_HIDDEN_ROWS][TG_TUI_SAVED_ROW_WIDTH];
    char saved_tail[TG_TUI_HIDDEN_ROWS][TG_TUI_SAVED_ROW_WIDTH];

#if defined(__AROS__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__MORPHOS) || defined(__amigaos3__) || defined(__m68k__)
    stream_path = "T:tg-tui-layout-self-test.tmp";
    (void)remove(stream_path);
    stream = fopen(stream_path, "w+");
#else
    stream_path = 0;
    stream = tmpfile();
#endif
    if (stream == 0) {
        puts("tui layout self-test: temporary stream unavailable");
        return 0;
    }
    saved_rows = tg_tui_rows;
    saved_columns = tg_tui_columns;
    saved_composer_rows = tg_tui_composer_rows;
    saved_hidden_count = tg_tui_hidden_row_count;
    saved_tail_count = tg_tui_tail_row_count;
    saved_active = tg_tui_active;
    saved_cache_valid = tg_tui_composer_cache_valid;
    saved_metrics = tg_tui_metrics;
    saved_cache = tg_tui_composer_cache_state;
    memcpy(saved_hidden, tg_tui_hidden_rows, sizeof(saved_hidden));
    memcpy(saved_tail, tg_tui_tail_rows, sizeof(saved_tail));

    tg_tui_rows = 14U;
    tg_tui_columns = 20U;
    tg_tui_composer_rows = 1U;
    tg_tui_hidden_row_count = 0U;
    tg_tui_tail_row_count = 0U;
    tg_tui_active = 1;
    tg_tui_composer_cache_valid = 0;
    memset(&tg_tui_metrics, 0, sizeof(tg_tui_metrics));
    strcpy(pending, "xxxx");
    length = 4UL;
    transitions = 0UL;

    tg_tui_tail_push("older");
    tg_tui_tail_push("newer");
    tg_tui_transition_composer(stream, 1U, 3U);
    tg_tui_transition_composer(stream, 3U, 1U);
    tail_ok = tg_tui_hidden_row_count == 0U &&
              tg_tui_tail_row_count == 2U &&
              strcmp(tg_tui_tail_rows[0], "older") == 0 &&
              strcmp(tg_tui_tail_rows[1], "newer") == 0;
    tg_tui_tail_clear();
    tg_tui_tail_push("older");
    tg_tui_tail_push("newer");
    tg_tui_composer_rows = 2U;
    tg_tui_hidden_row_count = 0U;
    tg_tui_transition_composer(stream, 2U, 3U);
    tg_tui_transition_composer(stream, 3U, 1U);
    tail_ok = tail_ok && tg_tui_hidden_row_count == 0U &&
              tg_tui_tail_row_count == 2U &&
              strcmp(tg_tui_tail_rows[0], "older") == 0 &&
              strcmp(tg_tui_tail_rows[1], "newer") == 0;
    tg_tui_composer_rows = 1U;
    tg_tui_hidden_row_count = 0U;
    tg_tui_tail_clear();
    tg_tui_composer_cache_valid = 0;
    tg_console_tui_input(stream, "", pending, length);
    memset(&tg_tui_metrics, 0, sizeof(tg_tui_metrics));

    while (length < 45UL) {
        old_rows = tg_tui_composer_rows;
        pending[length] = 'x';
        ++length;
        pending[length] = '\0';
        if (!tg_console_tui_input_append(stream, "", pending, length,
                                         'x')) {
            tg_console_tui_input(stream, "", pending, length);
        }
        if (tg_tui_composer_rows != old_rows) {
            ++transitions;
        }
    }
    while (length > 4UL) {
        old_rows = tg_tui_composer_rows;
        --length;
        pending[length] = '\0';
        if (!tg_console_tui_input_backspace(stream, "", pending,
                                            length)) {
            tg_console_tui_input(stream, "", pending, length);
        }
        if (tg_tui_composer_rows != old_rows) {
            ++transitions;
        }
    }
    ok = tail_ok && strcmp(pending, "xxxx") == 0 &&
         tg_tui_composer_rows == 1U &&
         transitions == 4UL && tg_tui_metrics.full_region_rows == 0UL &&
         tg_tui_metrics.transition_rows == 6UL &&
         tg_tui_metrics.fast_edits > 0UL &&
         tg_tui_metrics.slow_edits == 4UL &&
         tg_tui_metrics.composer_repaints == 8UL &&
         tg_tui_metrics.transition_rows +
                 tg_tui_metrics.composer_repaints <=
             16UL;
    test_metrics = tg_tui_metrics;
    final_composer_rows = tg_tui_composer_rows;

    tg_tui_rows = saved_rows;
    tg_tui_columns = saved_columns;
    tg_tui_composer_rows = saved_composer_rows;
    tg_tui_hidden_row_count = saved_hidden_count;
    tg_tui_tail_row_count = saved_tail_count;
    tg_tui_active = saved_active;
    tg_tui_composer_cache_valid = saved_cache_valid;
    tg_tui_metrics = saved_metrics;
    tg_tui_composer_cache_state = saved_cache;
    memcpy(tg_tui_hidden_rows, saved_hidden, sizeof(saved_hidden));
    memcpy(tg_tui_tail_rows, saved_tail, sizeof(saved_tail));
    fclose(stream);
    if (stream_path != 0) {
        (void)remove(stream_path);
    }
    if (!ok) {
        printf("tui layout self-test: incremental composer mismatch "
               "(transitions=%lu full=%lu changed=%lu composer=%lu "
               "fast=%lu slow=%lu final_rows=%u final_len=%lu tail=%d)\n",
               transitions, test_metrics.full_region_rows,
               test_metrics.transition_rows, test_metrics.composer_repaints,
               test_metrics.fast_edits, test_metrics.slow_edits,
               final_composer_rows, length, tail_ok);
    }
    return ok;
}

/* A message longer than the staging buffer must reach the transcript whole,
   split across logical lines with the wrap indent -- never truncated. */
static int tg_tui_line_push_self_test(void)
{
    static char source[901];
    static char rebuilt[1400];
    char line[512];
    FILE *stream;
    const char *stream_path;
    unsigned long i;
    unsigned long length;
    unsigned long start;
    unsigned long emitted;
    unsigned long pos;
    unsigned int saved_rows;
    unsigned int saved_columns;
    int saved_active;
    int ok;

#if defined(__AROS__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__MORPHOS) || defined(__amigaos3__) || defined(__m68k__)
    stream_path = "T:tg-tui-push-self-test.tmp";
    (void)remove(stream_path);
    stream = fopen(stream_path, "w+");
#else
    stream_path = 0;
    stream = tmpfile();
#endif
    if (stream == 0) {
        puts("tui layout self-test: temporary stream unavailable");
        return 0;
    }
    for (i = 0UL; i < 900UL; ++i) {
        source[i] = (i % 10UL) == 9UL
                        ? ' '
                        : (char)('a' + (int)((i / 10UL) % 26UL));
    }
    source[899] = 'z'; /* never end on a space: it would vanish in the split */
    source[900] = '\0';

    saved_rows = tg_tui_rows;
    saved_columns = tg_tui_columns;
    saved_active = tg_tui_active;
    tg_tui_rows = 14U;
    tg_tui_columns = 40U;
    tg_tui_active = 1;

    start = tg_tui_backlog_total;
    length = 0UL;
    for (i = 0UL; i < 900UL; ++i) {
        length = tg_console_tui_line_push(stream, line, sizeof(line), length,
                                          source[i]);
    }
    line[length] = '\0';
    tg_console_tui_line(stream, line);
    emitted = tg_tui_backlog_total - start;

    pos = 0UL;
    ok = emitted >= 2UL; /* 900 chars cannot fit one 512-byte line */
    for (i = 0UL; ok && i < emitted; ++i) {
        const char *part;

        part = tg_tui_backlog[(start + i) % TG_TUI_BACKLOG_LINES];
        if (i > 0UL) {
            unsigned int skipped;

            for (skipped = 0U; skipped < TG_TUI_WRAP_INDENT &&
                               *part == ' '; ++skipped) {
                ++part; /* the indent this split added */
            }
            if (skipped != TG_TUI_WRAP_INDENT) {
                ok = 0;
                break;
            }
            if (pos + 1UL < sizeof(rebuilt)) {
                rebuilt[pos++] = ' '; /* the space the break consumed */
            }
        }
        while (*part != '\0' && pos + 1UL < sizeof(rebuilt)) {
            rebuilt[pos++] = *part++;
        }
    }
    rebuilt[pos] = '\0';
    if (ok && strcmp(rebuilt, source) != 0) {
        ok = 0;
    }

    tg_tui_rows = saved_rows;
    tg_tui_columns = saved_columns;
    tg_tui_active = saved_active;
    tg_tui_composer_cache_valid = 0;
    fclose(stream);
    if (stream_path != 0) {
        (void)remove(stream_path);
    }
    if (!ok) {
        puts("tui layout self-test: long line lost text in the split");
    }
    return ok;
}

static int tg_tui_webpage_self_test(void)
{
    FILE *stream = tmpfile();
    FILE *capture;
    unsigned long start = tg_tui_backlog_total;
    unsigned long total;
    unsigned long i;
    unsigned int saved_rows = tg_tui_rows;
    unsigned int saved_columns = tg_tui_columns;
    unsigned int saved_composer = tg_tui_composer_rows;
    int saved_active = tg_tui_active;
    int ok;

    if (stream == 0) return 0;
    tg_tui_rows = 20U;
    tg_tui_columns = 80U;
    tg_tui_composer_rows = 1U;
    tg_tui_active = 1;
    capture = tg_console_tui_capture_begin(stream);
    fputs("pending link\n", capture);
    tg_console_tui_capture_webpage(capture, 7UL, 9UL, 0UL, 42UL);
    fputs("following message\n", capture);
    tg_console_tui_capture_end(capture, stream);
    total = tg_tui_backlog_total;
    ok = total == start + 2UL &&
        !tg_console_tui_complete_webpage(stream, 8UL,9UL,0UL,42UL,"unknown") &&
        !tg_console_tui_complete_webpage(stream, 7UL,9UL,0UL,43UL,"wrong channel") &&
        tg_tui_backlog_total == total &&
        tg_console_tui_complete_webpage(stream, 7UL,9UL,0UL,42UL,
            "[Link: Test site - Late title]\nFirst description.") &&
        tg_tui_backlog_total == total + 2UL &&
        strcmp(tg_tui_backlog[start % TG_TUI_BACKLOG_LINES], "pending link") == 0 &&
        strcmp(tg_tui_backlog[(start+1UL) % TG_TUI_BACKLOG_LINES],
               "[Link: Test site - Late title]") == 0 &&
        strcmp(tg_tui_backlog[(start+2UL) % TG_TUI_BACKLOG_LINES],
               "First description.") == 0 &&
        strcmp(tg_tui_backlog[(start+3UL) % TG_TUI_BACKLOG_LINES],
               "following message") == 0 &&
        !tg_console_tui_complete_webpage(stream, 7UL,9UL,0UL,42UL,"duplicate");
    /* The id leaves with its cache slot; it must never attach to the new row. */
    capture = tg_console_tui_capture_begin(stream);
    fputs("evicted link\n", capture);
    tg_console_tui_capture_webpage(capture, 7UL, 10UL, 0UL, 0UL);
    tg_console_tui_capture_end(capture, stream);
    for (i = 0UL; i < TG_TUI_BACKLOG_LINES; ++i) {
        tg_console_tui_line(stream, "new row");
    }
    total = tg_tui_backlog_total;
    ok = ok && !tg_console_tui_complete_webpage(stream, 7UL,10UL,0UL,0UL,"stale") &&
        tg_tui_backlog_total == total;
    fclose(stream);
    tg_tui_active = saved_active;
    tg_tui_rows = saved_rows;
    tg_tui_columns = saved_columns;
    tg_tui_composer_rows = saved_composer;
    if (!ok) puts("tui layout self-test: delayed webpage cache failed");
    return ok;
}

int tg_console_tui_layout_self_test(void)
{
    static const char words[] =
        "0123456789 1234567890 1234567890 1234567890 1234567890 "
        "1234567890";
    static const char coloured[] =
        TG_UI_CSI "31m0123456789 1234567890 1234567890 1234567890 "
        "1234567890 1234567890" TG_UI_CSI "0m";
    char monster[51];
    char composer[161];
    tg_tui_composer_plan plan;
    unsigned int i;

    if (!tg_tui_expect_wrap(
            "40 columns", words, 40U,
            "0123456789 1234567890 1234567890|  1234567890 1234567890 "
            "1234567890") ||
        !tg_tui_expect_wrap(
            "53 columns", words, 53U,
            "0123456789 1234567890 1234567890 1234567890|  1234567890 "
            "1234567890") ||
        !tg_tui_expect_wrap("80 columns", words, 80U, words) ||
        !tg_tui_expect_wrap(
            "colour width", coloured, 40U,
            "0123456789 1234567890 1234567890|  1234567890 1234567890 "
            "1234567890")) {
        return 2;
    }
    for (i = 0U; i < 50U; ++i) {
        monster[i] = 'x';
    }
    monster[50] = '\0';
    if (!tg_tui_expect_wrap(
            "hard word", monster, 40U,
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx|  xxxxxxxxxxx")) {
        return 2;
    }

    for (i = 0U; i < 160U; ++i) {
        composer[i] = 'x';
    }
    composer[160] = '\0';
    composer[10] = '\0';
    tg_tui_make_composer_plan(composer, 10UL, 39U, &plan);
    if (plan.rows != 1U || plan.caret_piece != 0UL ||
        plan.caret_column != 11U) {
        puts("tui layout self-test: composer one-row mismatch");
        return 2;
    }
    composer[10] = 'x';
    composer[50] = '\0';
    tg_tui_make_composer_plan(composer, 50UL, 39U, &plan);
    if (plan.rows != 2U || plan.caret_piece != 1UL) {
        puts("tui layout self-test: composer two-row mismatch");
        return 2;
    }
    composer[50] = 'x';
    composer[90] = '\0';
    tg_tui_make_composer_plan(composer, 90UL, 39U, &plan);
    if (plan.rows != 3U || plan.caret_piece != 2UL) {
        puts("tui layout self-test: composer three-row mismatch");
        return 2;
    }
    for (i = 90U; i < 160U; ++i) {
        composer[i] = 'x';
    }
    composer[160] = '\0';
    tg_tui_make_composer_plan(composer, 160UL, 39U, &plan);
    if (plan.rows != 3U || plan.total_pieces != 5UL ||
        plan.first_piece != 2UL || plan.caret_piece != 4UL) {
        puts("tui layout self-test: composer tail viewport mismatch");
        return 2;
    }
    tg_tui_make_composer_plan("short", 5UL, 39U, &plan);
    if (plan.rows != 1U || plan.first_piece != 0UL) {
        puts("tui layout self-test: composer shrink mismatch");
        return 2;
    }
    /* 0.0.95: a message near Telegram's limit is laid out whole (the text
       used to stop at 640 bytes, prompt included), and the three rows
       shown are the ones around the caret at its end. */
    {
        static char pending[4001];
        static char built[TG_TUI_INPUT_TEXT_MAX];
        unsigned long caret = 0UL;
        unsigned long length;

        for (i = 0U; i < 4000U; ++i) {
            pending[i] = (char)((i % 10U) == 9U ? ' ' : 'a' + (int)(i % 26U));
        }
        pending[4000] = '\0';
        length = tg_tui_build_input_text(built, "Me> ", pending, 4000UL,
                                         4000UL, &caret);
        tg_tui_make_composer_plan(built, caret, 39U, &plan);
        if (length != 4004UL || caret != 4004UL || plan.rows != 3U ||
            plan.caret_piece + 1UL != plan.total_pieces ||
            plan.first_piece + 3UL != plan.total_pieces) {
            puts("tui layout self-test: a long message does not fit the composer");
            return 2;
        }
        /* a pasted line break: one pilcrow cell, the caret after it */
        length = tg_tui_build_input_text(built, "> ", "ab\ncd", 5UL, 3UL,
                                         &caret);
        if (length != 7UL || built[4] != (char)0xB6 || caret != 5UL) {
            puts("tui layout self-test: a line break does not show as one cell");
            return 2;
        }
    }
    if (!tg_tui_composer_incremental_self_test()) {
        return 2;
    }
    if (!tg_tui_line_push_self_test()) {
        return 2;
    }
    if (!tg_tui_webpage_self_test()) {
        return 2;
    }
    puts("tui layout self-test: ok (wrap + incremental composer + long lines + webpages)");
    return 0;
}

int tg_console_tui_self_test(FILE *stream)
{
    char line[64];
    char ch;
    unsigned int i;
    unsigned int rows;
    unsigned int columns;
    int raw_ok;

    if (stream == 0) {
        return 2;
    }
    raw_ok = tg_platform_stdin_set_raw(1) == 0;
    if (!raw_ok) {
        fputs("tui-test: raw console mode unavailable\n", stream);
        return 2;
    }
    tg_console_ui_set_interactive(1);
    if (!tg_console_tui_query_size(stream, &rows, &columns)) {
        tg_platform_stdin_set_raw(0);
        fputs("tui-test: window size query failed\n", stream);
        return 2;
    }
    fprintf(stream, "tui-test: window %ux%u, entering in 2s...\n", columns,
            rows);
    fflush(stream);
    (void)tg_platform_stdin_read_char(2UL, &ch);
    if (!tg_console_tui_enter(stream,
                              " Telegram Amiga -- TUI test (q quits) ")) {
        tg_platform_stdin_set_raw(0);
        fputs("tui-test: enter failed\n", stream);
        return 2;
    }
    for (i = 1U; i <= 30U; ++i) {
        sprintf(line, "transcript line %u of 30", i);
        tg_console_tui_line(stream, line);
    }
    tg_console_tui_line(stream, "resize the window to test NEWSIZE");
    tg_console_tui_input(stream, "you: ", "type q to quit", 14UL);
    for (;;) {
        if (tg_platform_stdin_read_char(60UL, &ch) <= 0) {
            break;
        }
        if (ch == 'q' || ch == 'Q' || ch == 3) {
            break;
        }
        if (ch == (char)0x9b || ch == (char)0x1b) {
            /* Swallow the CSI sequence; react to the NEWSIZE event. */
            unsigned long event_class;
            int byte_value;

            if (ch == (char)0x1b) {
                if (tg_platform_stdin_read_char(1UL, &ch) <= 0 ||
                    ch != '[') {
                    continue;
                }
            }
            event_class = 0UL;
            byte_value = 0;
            for (;;) {
                if (tg_platform_stdin_read_char(1UL, &ch) <= 0) {
                    break;
                }
                byte_value = (int)(unsigned char)ch;
                if (byte_value >= '0' && byte_value <= '9' &&
                    event_class < 1000UL) {
                    event_class = (event_class * 10UL) +
                                  (unsigned long)(byte_value - '0');
                    continue;
                }
                if (byte_value >= 0x40 && byte_value <= 0x7e) {
                    break;
                }
                if (byte_value == ';') {
                    /* Stop accumulating: only the class matters. */
                    event_class += 1000UL;
                }
            }
            if (byte_value == '|' && (event_class % 1000UL) == 12UL) {
                if (tg_console_tui_resize(stream,
                                          " TUI test -- resized ")) {
                    sprintf(line, "resized: new layout drawn");
                    tg_console_tui_line(stream, line);
                }
            }
            tg_console_tui_input(stream, "you: ", "type q to quit", 14UL);
            continue;
        }
        sprintf(line, "key 0x%02x", (unsigned char)ch);
        tg_console_tui_line(stream, line);
        tg_console_tui_input(stream, "you: ", "type q to quit", 14UL);
    }
    tg_console_tui_leave(stream);
    tg_platform_stdin_set_raw(0);
    fputs("tui-test: done\n", stream);
    return 0;
}
#endif /* !TG_NO_SELFTEST */
