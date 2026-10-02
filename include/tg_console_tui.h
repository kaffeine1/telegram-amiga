/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Full-screen console TUI for the interactive chat: a status bar pinned to
 * the top row, a transcript region that scrolls in the middle and an input
 * line pinned to the bottom row -- the AmIRC-style layout, built only on
 * sequences every Amiga-family console implements.
 *
 * Amiga consoles have no ANSI scroll regions, so the transcript scrolls via
 * the classic insert/delete-line trick: delete the region's top row (rows
 * below shift up, the input row included), then insert a blank row just
 * above the input row (pushing it back down). The window size comes from
 * the console WINDOW STATUS REQUEST (CSI 0 SP q), answered on the input
 * stream as CSI 1;1;<rows>;<cols> SP r.
 */

#ifndef TG_CONSOLE_TUI_H
#define TG_CONSOLE_TUI_H

/* The longest message the text client composes: Telegram's limit for one
   message, 4096 characters (one byte each in the Amiga's Latin-1). */
#define TG_CONSOLE_TUI_MESSAGE_MAX 4096U

#include <stdio.h>

/* Master switch: when disabled, tg_console_tui_enter always declines and
   the chat stays on the linear flow (--ui-tui off). */
void tg_console_tui_set_enabled(int enabled);

/* Queries the console window size in characters via CSI 0 SP q. Requires
   stdin in raw mode. Returns 1 and fills rows/columns on success. */
int tg_console_tui_query_size(FILE *stream,
                              unsigned int *rows,
                              unsigned int *columns);

/* Enters the full-screen layout: clears the window, draws the status bar
   and the input separator, homes the input line. Requires raw stdin and a
   window at least 8 rows by 20 columns. Returns 1 when active. */
int tg_console_tui_enter(FILE *stream, const char *status_text);

/* 1 while the full-screen layout is active. */
int tg_console_tui_active(void);

/* Rewrites the top status bar (clipped to the window width). */
void tg_console_tui_status(FILE *stream, const char *status_text);

/* Appends one logical line (no newline) to the transcript region. It wraps by
   words at paint time, so resize reflows the backlog without losing text.
   Bytes are written as-is and colour role sequences consume no columns. */
void tg_console_tui_line(FILE *stream, const char *text);

/* Redraws prompt + pending input. The composer grows to three video rows and
   then shows a bounded tail viewport. The ordinary entry point places the
   caret at the end; the explicit variant keeps an editor caret coherent across
   wrapped rows. */
void tg_console_tui_input(FILE *stream,
                          const char *prompt,
                          const char *pending,
                          unsigned long pending_length);
void tg_console_tui_input_caret(FILE *stream,
                                const char *prompt,
                                const char *pending,
                                unsigned long pending_length,
                                unsigned long pending_caret);

/* Appends one character to a transcript staging buffer, flushing it as a
   transcript line (at a word boundary when possible, remainder carried over
   with the wrap indent) instead of dropping the tail when it is full.
   Returns the new length. */
unsigned long tg_console_tui_line_push(FILE *stream, char *line,
                                       unsigned long capacity,
                                       unsigned long length, char ch);

/* Flicker-free caret-at-end fast paths (slow 68000 consoles): echo one new
   character / rub one out without the full row repaint. Return 1 when done,
   0 when the caller must fall back to tg_console_tui_input(). The length is
   the pending length AFTER the edit. */
int tg_console_tui_input_append(FILE *stream,
                                const char *prompt,
                                const char *pending,
                                unsigned long pending_length,
                                char ch);
int tg_console_tui_input_backspace(FILE *stream,
                                   const char *prompt,
                                   const char *pending,
                                   unsigned long pending_length);

/* Leaves the layout: moves below the status area and restores attributes. */
void tg_console_tui_leave(FILE *stream);

/* Captures printer output for the transcript region: begin returns a
   temporary stream to print into; end splits what was written into lines
   and feeds each to tg_console_tui_line, then disposes the stream. When the
   TUI is not active, begin returns the fallback stream itself and end is a
   no-op -- so call sites work unchanged in linear mode. */
FILE *tg_console_tui_capture_begin(FILE *fallback);
void tg_console_tui_capture_end(FILE *capture, FILE *fallback);

/* Associate the just-rendered message with its pending preview. The mark is
   out of band: no protocol bytes are written into the visible transcript. */
void tg_console_tui_capture_webpage(FILE *capture,
                                    unsigned long page_hi, unsigned long page_lo,
                                    unsigned long channel_hi, unsigned long channel_lo);
/* Add completed preview lines next to every matching cached message. The
   text is already in the console display encoding. Unknown ids do nothing. */
int tg_console_tui_complete_webpage(FILE *stream,
                                   unsigned long page_hi, unsigned long page_lo,
                                   unsigned long channel_hi, unsigned long channel_lo,
                                   const char *text);

/* Remembers the prompt text the input row should show; the line editor
   redraws the row with it after every keystroke while the TUI is active. */
void tg_console_tui_set_prompt(const char *prompt);
const char *tg_console_tui_prompt(void);

/* Window-resize handling. enter() subscribes to the console's NEWSIZE raw
   event (CSI 12 {); the line editor calls note_resize() when the event
   report (CSI 12;...|) shows up in the input stream; the chat loop polls
   resize_pending() and calls resize(), which re-queries the window size and
   repaints the chrome and replays the recent transcript lines from the
   in-memory backlog. leave() unsubscribes. */
void tg_console_tui_note_resize(void);
int tg_console_tui_resize_pending(void);
int tg_console_tui_resize(FILE *stream, const char *status_text);

/* Scrollback: pages the transcript view by logical messages through the
   in-memory backlog. Their video-row count is recalculated at paint time.
   direction > 0 goes back in time, < 0 toward live; the separator row doubles
   as the indicator and any chrome repaint returns to live. */
void tg_console_tui_scroll(FILE *stream, int direction);

/* Automated pure-layout golden used by the chat-render self-test. */
int tg_console_tui_layout_self_test(void);

/* Interactive diagnostic for --console-tui-test. */
int tg_console_tui_self_test(FILE *stream);

#endif
