/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_CLIENT_STATE_H
#define TG_CLIENT_STATE_H

/**
 * Callback used while iterating one-line chat state rows.
 *
 * index is 1-based and follows file order. line is a borrowed view without the
 * trailing newline and remains valid only during the callback.
 */
typedef int (*tg_client_state_line_callback)(unsigned long index,
                                             const char *line,
                                             unsigned long line_length,
                                             void *context);

/**
 * Builds one sanitized chat-state row:
 *
 *   <chat-id>|<sender>|<date>|<text>\n
 *
 * sender/date/text are copied and sanitized so embedded separators or line
 * breaks cannot corrupt the one-line file format.
 */
int tg_client_state_build_line(const char *chat_id,
                               const char *sender,
                               const char *date,
                               const char *text,
                               char *buffer,
                               unsigned long buffer_size);

/**
 * Rewrites path so new_line becomes the first row and any older row for chat_id
 * is removed. Missing files are treated as empty state. Returns 0 on success.
 */
int tg_client_state_update_file_line(const char *path,
                                     const char *chat_id,
                                     const char *new_line,
                                     int verbose);

/**
 * Parses one chat-state row into caller-owned buffers.
 */
int tg_client_state_parse_line(const char *line,
                               unsigned long line_length,
                               char *chat_id,
                               unsigned long chat_id_size,
                               char *sender,
                               unsigned long sender_size,
                               char *date,
                               unsigned long date_size,
                               char *text,
                               unsigned long text_size);

/**
 * Iterates chat-state rows from path. When missing_ok is non-zero, a missing
 * file prints "telegram chats: none" and returns success.
 */
int tg_client_state_for_each_line(const char *path,
                                  tg_client_state_line_callback callback,
                                  void *context,
                                  int missing_ok);

/**
 * Resolves a 1-based chat list index to a chat id copied into chat_id.
 */
int tg_client_state_find_chat_id_by_index(const char *path,
                                          unsigned long wanted_index,
                                          char *chat_id,
                                          unsigned long chat_id_size);

#endif
