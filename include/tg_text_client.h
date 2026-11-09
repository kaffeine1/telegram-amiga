/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_TEXT_CLIENT_H
#define TG_TEXT_CLIENT_H

typedef struct tg_text_client_config tg_text_client_config;

typedef int (*tg_text_client_poll_callback)(
    const tg_text_client_config *client_config,
    void *callback_context);

typedef int (*tg_text_client_send_chat_callback)(
    const tg_text_client_config *client_config,
    const char *chat_id,
    const char *text,
    int verbose,
    void *callback_context);

typedef int (*tg_text_client_send_index_callback)(
    const tg_text_client_config *client_config,
    const char *index_text,
    const char *text,
    void *callback_context);

struct tg_text_client_config {
    const char *token_file_path;
    const char *offset_file_path;
    const char *inbox_log_file_path;
    const char *chat_state_file_path;
    const char *selected_chat_file_path;
    const char *poll_seconds_text;
    const char *max_iterations_text;
    tg_text_client_poll_callback poll_once;
    tg_text_client_send_chat_callback send_chat_id;
    tg_text_client_send_index_callback send_chat_index;
    void *callback_context;
};

int tg_text_client_run(const tg_text_client_config *client_config);
int tg_text_client_run_human(const tg_text_client_config *client_config);
int tg_text_client_print_last_inbox_line(const char *inbox_log_file_path);
int tg_text_client_print_chats(const char *chat_state_file_path);
void tg_text_client_print_status(const tg_text_client_config *client_config);
int tg_text_client_self_test(void);

#endif
