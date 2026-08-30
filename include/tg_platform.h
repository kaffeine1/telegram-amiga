/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_PLATFORM_H
#define TG_PLATFORM_H

#include "tg_net.h"
#include "tg_tls.h"

#if defined(TG_LOWMEM)
/* 68000 LOWMEM build for 4 MB fast-RAM machines. The full megabyte was only
   ever needed by the first login (the DH exchange), which this package tells
   users to perform on a faster machine; with the GUI compiled out as well,
   the text client's deepest paths are the MTProto parsers. 384 KB leaves
   them a wide margin and hands a further 128 KB back to a machine whose
   WiFi driver has already taken most of its fast RAM. */
#define TG_PLATFORM_SAFE_STACK_SIZE 393216UL
#define TG_PLATFORM_SAFE_STACK_MIN 327680UL
#define TG_PLATFORM_SAFE_STACK_COOKIE "$STACK:393216"
#else
#define TG_PLATFORM_SAFE_STACK_SIZE 1048576UL
#define TG_PLATFORM_SAFE_STACK_MIN 819200UL
#define TG_PLATFORM_SAFE_STACK_COOKIE "$STACK:1048576"
#endif

typedef int (*tg_platform_entry_fn)(int argc, char **argv);

/**
 * Runs the application entry point with enough stack for the GUI, crypto and
 * large MTProto reply parsers. Amiga runtimes with a native minimum-stack
 * mechanism validate it here; AROS can fall back to NewStackSwap(). Hosts pass
 * straight through.
 */
int tg_platform_run_with_safe_stack(tg_platform_entry_fn entry,
                                    int argc, char **argv);

/**
 * Returns the human-readable platform name.
 *
 * The returned string is static and must not be freed.
 */
const char *tg_platform_name(void);

/**
 * Returns the default application data directory for the platform.
 *
 * The returned string is static and must not be freed.
 */
const char *tg_platform_default_data_dir(void);

/**
 * Emits one log message through the platform-specific logging backend.
 *
 * level and message are borrowed for the duration of the call.
 */
void tg_platform_log(const char *level, const char *message);

/**
 * Emits one line to the platform's low-level debug output (the serial / kernel
 * debug channel a tool like Sashimi captures on MorphOS). Unlike a file or
 * stdout it is unbuffered and survives a hard freeze, so it is used for the
 * crash-safe GUI lifecycle log. A no-op where there is no such channel.
 */
void tg_platform_debug(const char *message);

/**
 * Suspends execution for approximately the requested number of seconds.
 *
 * A value of zero returns immediately. This is used only by bounded polling
 * commands; platform backends may use the simplest native sleep primitive.
 */
void tg_platform_sleep_seconds(unsigned long seconds);

/**
 * Folds one user-input event (key press, mouse move/click) into the platform's
 * keystroke-timing entropy ring, which the DRBG absorbs on every generate.
 * `a` and `b` are opaque event words (e.g. IDCMP class^code and packed mouse
 * coordinates); the platform mixes in its own fine-grained timer at call time,
 * which is where most of the entropy lives. O(1), no hashing on the input
 * path -- safe to call for every event. Closes the gap where the GUI (the
 * primary shipped front-end) fed no input entropy before the first-run
 * auth-key DH, while the console stdin path did.
 */
void tg_platform_note_input_event(unsigned long a, unsigned long b);

/* Best-effort: give `drawer` a Workbench icon (<drawer>.info with the system
   default drawer image) so a program-created directory -- downloads/ -- is
   visible to the user. No-op where icons do not exist (host) or on failure. */
void tg_platform_ensure_drawer_icon(const char *drawer);

/**
 * Returns the local wall-clock time as a Unix-style epoch (seconds since
 * 1970-01-01), read straight from the Amiga system clock so it matches what the
 * Workbench clock shows -- with NO timezone or DST adjustment applied. Used to
 * anchor message-time display on the real wall clock instead of C time(), which
 * on some Amiga toolchains (clib2) reports UTC via the locale GMT offset and
 * never applies DST. On non-Amiga hosts it falls back to time(0).
 */
unsigned long tg_platform_local_epoch(void);

/**
 * Waits until standard input appears readable, or until timeout_seconds pass.
 *
 * Returns non-zero when a line can probably be read with fgets(), zero on
 * timeout or when the platform cannot test standard input readiness. A timeout
 * of zero performs a non-blocking poll.
 */
int tg_platform_stdin_readable(unsigned long timeout_seconds);

/**
 * Reads one character from standard input after waiting up to timeout_seconds.
 *
 * Returns 1 when one character was read, 0 on timeout and -1 when input is
 * closed or failed. Unlike tg_platform_stdin_readable()+fgets(), this lets
 * interactive loops keep polling while the user has not completed a line yet.
 */
int tg_platform_stdin_read_char(unsigned long timeout_seconds, char *out_char);

/**
 * Reads one line from standard input WITHOUT echoing it (for secrets such as a
 * 2FA password). The trailing newline is not stored and out is NUL terminated.
 *
 * Returns 0 on success and -1 when input is closed or failed. On consoles that
 * cannot suppress echo the platform falls back to a normal read, so callers
 * must not assume the input was hidden on every target.
 */
int tg_platform_stdin_read_hidden_line(char *out, unsigned long out_size);

/**
 * Switches standard input between raw (no echo, no line editing, one keystroke
 * at a time) and the normal cooked mode. Used by the interactive chat to do its
 * own line editing and command-history recall.
 *
 * Returns 0 when the mode was changed and -1 when the console cannot be put in
 * raw mode; callers must fall back to plain line input on -1.
 */
int tg_platform_stdin_set_raw(int enabled);

/**
 * Fills bytes with platform-provided random data.
 *
 * Returns non-zero on success. A zero return means the platform backend does
 * not currently expose a suitable source; callers that create persistent
 * secrets must fail closed in that case.
 */
int tg_platform_random_bytes(unsigned char *bytes, unsigned long byte_count);

/**
 * Prepare the process for a Workbench (icon double-click) launch.
 *
 * Called once from main() when the program was started with no CLI (argc == 0,
 * the Amiga C-runtime convention for a Workbench start). On the Amiga family
 * this CurrentDir()s to PROGDIR: so the relative data files (telegram-api.txt,
 * telegram-auth.bin, telegram-peers.txt, ...) resolve next to the binary even
 * though Workbench did not set the drawer as the working directory. No-op on
 * the host build and anywhere a working directory is already correct.
 */
void tg_platform_workbench_init(void);

/**
 * Releases process-lifetime platform resources opened lazily by the client.
 *
 * Called exactly once by main() after the application has stopped. Backends
 * with no such resources implement this as a no-op.
 */
void tg_platform_shutdown(void);

/* Workbench-launched TUI: open an interactive CON: window and make it this
   process's stdin/stdout, since a double-clicked binary has no console.
   1 = a console is in place, 0 = not available (host / failure). */
int tg_platform_workbench_tui_console(void);

/* Teardown mirror of the above, called after the console client returns: puts
   the original process streams and console task back and CLOSES our CON:
   handle. Without it the con-handler keeps the window alive forever -- the
   close gadget could never dismiss it after quit. No-op on the host build or
   when the console was never opened. */
void tg_platform_workbench_tui_console_close(void);

/* Open `url` in the system browser (0.0.8 clickable links). Amiga lanes run
   the OpenURL/URLOpen shell command synchronously (NIL: I/O) and report its
   rc; hosts without one return non-zero so the caller can fall back to
   copying the URL to the clipboard. */
int tg_platform_open_url(const char *url);

/* Reads the TUI_MODE tooltype of the icon that launched us (WBStartup arg 0)
   so a packager can pick the front-end explicitly (issue #9). Returns 1 when
   TUI_MODE is present and true, 0 when present and NO/FALSE/OFF, and -1 when
   absent / no icon / not applicable -- the caller then falls back to the
   filename heuristic, keeping the default byte-identical icons working.
   `argv` is main()'s argv, i.e. the WBStartup pointer on a Workbench launch. */
int tg_platform_wb_tui_mode(char **argv);

/* Drag-and-drop for the Workbench TUI console: a file icon dropped on the
   window delivers its path so the user can just drop it after "/sendfile ".
   Non-blocking poll called from the console read loop; returns 1 with a NUL-
   terminated path in `out` when a file was dropped, else 0. Arming/disarming
   is folded into the console open/close. Only AmigaOS 4 implements it (via an
   AppWindow whose Window pointer is validated against the screen window list
   before use, so a bad handle degrades to "no drop" rather than a crash);
   every other build is a no-op that returns 0. */
int tg_platform_console_drop_poll(char *out, unsigned long out_size);

/* One-line, human-readable status of the drag-and-drop arming ("ready", or
   which step bailed). Printed at console startup so a field report can say
   WHY drops are inactive without a debug build. */
const char *tg_platform_console_drop_diag(void);

/* GUI drag-and-drop (0.0.8 punto 1e): arm the SAME drop machinery on the
   GUI's own Intuition window (an icon dropped on it uploads the file to the
   open chat). arm returns 0 when the AppWindow is registered; disarm is
   idempotent and must run before the window closes; sigmask returns the
   drop port's wait mask (0 when unarmed) for the event loop's Wait(); the
   dropped paths come out of the same tg_platform_console_drop_poll. One
   owner at a time process-wide: the TUI console drop or the GUI, never
   both. Hosts without Workbench are no-ops. */
int tg_platform_gui_drop_arm(void *window);
void tg_platform_gui_drop_disarm(void);
unsigned long tg_platform_gui_drop_sigmask(void);

/*
 * Returns non-zero when the user asked to abort (Amiga family: the shell
 * break signal SIGBREAKF_CTRL_C, left pending so the caller's main loop can
 * also see it). Network wait loops poll this so a stalled connection can be
 * abandoned with Ctrl+C instead of requiring a reboot.
 */
int tg_platform_break_pending(void);

/*
 * Flashes the display (Intuition DisplayBeep): the user-visible "bell" that
 * never touches the console byte stream. Console handlers disagree wildly
 * about the BEL byte -- AmiKit's replacement console reacts by clearing the
 * window, which tore the full-screen chat apart on a notification.
 */
void tg_platform_display_beep(void);

/*
 * Clear the AmigaDOS "e" protection bit on a saved file, so a program a
 * friend sent through a chat runs without a manual `protect +e` (issue #15).
 * The RWED bits are ACTIVE LOW: a set FIBB_EXECUTE forbids execution, so
 * making a file runnable means clearing it. Read-modify-write, so the
 * archive bit and the rest survive. A no-op wherever there is no AmigaDOS.
 */
void tg_platform_set_executable(const char *path);

/**
 * Platform TCP connect implementation used by tg_net_connect().
 *
 * On success, connection must contain a valid platform_handle and is_open must
 * be non-zero. error_buffer is caller-owned and optional.
 */
tg_net_status tg_platform_tcp_connect(tg_net_connection *connection, const char *host,
                                      const char *port, char *error_buffer,
                                      unsigned long error_buffer_size);

/**
 * Platform TCP send implementation used by tg_net_send().
 */
tg_net_status tg_platform_tcp_send(tg_net_connection *connection, const void *data,
                                   unsigned long byte_count, unsigned long *bytes_sent,
                                   char *error_buffer, unsigned long error_buffer_size);

/**
 * Platform TCP receive implementation used by tg_net_recv().
 */
tg_net_status tg_platform_tcp_recv(tg_net_connection *connection, void *buffer,
                                   unsigned long buffer_size, unsigned long *bytes_received,
                                   char *error_buffer, unsigned long error_buffer_size);

/**
 * Non-blocking TCP readability test used by live-session push drains.
 *
 * Returns 1 when data (or EOF) is ready, 0 when no data is queued, and -1 on
 * error. Implementations must not wait for network traffic.
 */
int tg_platform_tcp_poll_readable(tg_net_connection *connection,
                                  char *error_buffer,
                                  unsigned long error_buffer_size);

/**
 * Platform TCP close implementation used by tg_net_close().
 */
void tg_platform_tcp_close(tg_net_connection *connection);

/**
 * Platform TLS connect implementation used by tg_tls_connect().
 *
 * Backends should set net_status when the failure originates in the TCP layer.
 */
tg_tls_status tg_platform_tls_connect(tg_tls_connection *connection, const char *host,
                                      const char *port, tg_net_status *net_status,
                                      char *error_buffer, unsigned long error_buffer_size);

/**
 * Platform TLS send implementation used by tg_tls_send().
 */
tg_tls_status tg_platform_tls_send(tg_tls_connection *connection, const void *data,
                                   unsigned long byte_count, unsigned long *bytes_sent,
                                   char *error_buffer, unsigned long error_buffer_size);

/**
 * Platform TLS receive implementation used by tg_tls_recv().
 */
tg_tls_status tg_platform_tls_recv(tg_tls_connection *connection, void *buffer,
                                   unsigned long buffer_size, unsigned long *bytes_received,
                                   char *error_buffer, unsigned long error_buffer_size);

/**
 * Platform TLS close implementation used by tg_tls_close().
 */
void tg_platform_tls_close(tg_tls_connection *connection);

#endif
