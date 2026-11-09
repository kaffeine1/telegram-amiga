/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_CONSOLE_H
#define TG_CONSOLE_H

/**
 * Trims leading and trailing ASCII whitespace from a mutable command line.
 *
 * The buffer is modified in place and no allocation is performed.
 */
void tg_console_trim_ascii_space(char *text);

/**
 * Parses a console reply/send command suffix.
 *
 * line must contain the full command line and command_length is the length of
 * the already matched command prefix. index_buffer receives the decimal chat
 * index as a NUL-terminated string. text_start is set to a borrowed pointer
 * inside line for the message text. Returns 0 on success.
 */
int tg_console_parse_reply_command(char *line, unsigned long command_length,
                                   char *index_buffer,
                                   unsigned long index_buffer_size,
                                   char **text_start);

/**
 * Parses a console command that contains only a decimal chat index after the
 * command prefix. Returns 0 on success.
 */
int tg_console_parse_index_command(char *line, unsigned long command_length,
                                   char *index_buffer,
                                   unsigned long index_buffer_size);

/**
 * Parses a console watch command.
 *
 * line must contain the full command line. Both "/watch" and "watch" prefixes
 * are accepted so chat mode and the top-level console can share the parser. On
 * success watch_seconds receives the requested interval in seconds, or 0 for
 * "off". Values above 3600 seconds are rejected. The buffer is only read and no
 * allocation is performed.
 */
int tg_console_parse_watch_command(const char *line,
                                   unsigned long *watch_seconds);

#endif
