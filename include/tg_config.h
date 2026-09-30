/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_CONFIG_H
#define TG_CONFIG_H

#include <stdio.h>

#include "tg_log.h"

/**
 * Parsed command-line configuration.
 *
 * String fields point directly into argv or static platform strings. The config
 * object does not allocate or own those strings.
 */
typedef struct tg_config {
    const char *data_dir;
    const char *token_file_path_override;
    const char *connect_timeout_seconds;
    const char *tls_ca_file;
    const char *tls_ca_path;
    const char *inbox_log_file_path;
    const char *chat_state_file_path;
    const char *net_test_host;
    const char *net_test_port;
    const char *http_test_host;
    const char *http_test_port;
    const char *http_test_path;
    const char *https_test_host;
    const char *https_test_port;
    const char *https_test_path;
    const char *mtproto_probe_host;
    const char *mtproto_probe_port;
    const char *mtproto_probe_dc_id;
    const char *mtproto_auth_host;
    const char *mtproto_auth_port;
    const char *mtproto_auth_dc_id;
    const char *mtproto_auth_api_id;
    const char *mtproto_auth_api_hash;
    const char *mtproto_auth_api_file;
    const char *mtproto_auth_phone;
    const char *mtproto_auth_file;
    const char *mtproto_auth_code_hash_file;
    const char *mtproto_auth_code;
    const char *mtproto_auth_first_name;
    const char *mtproto_auth_last_name;
    const char *mtproto_auth_password_file;
    const char *mtproto_auth_limit;
    const char *mtproto_auth_message;
    const char *mtproto_auth_peer_cache_file;
    const char *mtproto_auth_peer_index;
    const char *mtproto_auth_username;
    const char *mtproto_chat_peer_cache_file;
    const char *json_test_input;
    const char *json_test_field;
    const char *telegram_json_test_input;
    const char *telegram_path_test_token;
    const char *telegram_path_test_method;
    const char *telegram_token_file_path;
    const char *telegram_token_file_method;
    const char *telegram_default_token_file_method;
    const char *telegram_get_me_token_file_path;
    const char *telegram_get_updates_token_file_path;
    const char *telegram_get_updates_offset;
    const char *telegram_get_updates_default_offset;
    const char *telegram_read_once_state_token_file_path;
    const char *telegram_read_once_state_offset_file_path;
    const char *telegram_read_once_state_default_offset_file_path;
    const char *telegram_read_loop_token_file_path;
    const char *telegram_read_loop_offset_file_path;
    const char *telegram_read_loop_poll_seconds;
    const char *telegram_read_loop_max_iterations;
    const char *telegram_read_loop_default_offset_file_path;
    const char *telegram_read_loop_default_poll_seconds;
    const char *telegram_read_loop_default_max_iterations;
    const char *telegram_inbox_token_file_path;
    const char *telegram_inbox_offset_file_path;
    const char *telegram_inbox_default_offset_file_path;
    const char *telegram_inbox_loop_token_file_path;
    const char *telegram_inbox_loop_offset_file_path;
    const char *telegram_inbox_loop_poll_seconds;
    const char *telegram_inbox_loop_max_iterations;
    const char *telegram_inbox_loop_default_offset_file_path;
    const char *telegram_inbox_loop_default_poll_seconds;
    const char *telegram_inbox_loop_default_max_iterations;
    const char *telegram_session_token_file_path;
    const char *telegram_session_offset_file_path;
    const char *telegram_session_inbox_log_file_path;
    const char *telegram_session_chat_state_file_path;
    const char *telegram_session_default_offset_file_path;
    const char *telegram_session_default_inbox_log_file_path;
    const char *telegram_session_default_chat_state_file_path;
    const char *telegram_session_loop_token_file_path;
    const char *telegram_session_loop_offset_file_path;
    const char *telegram_session_loop_inbox_log_file_path;
    const char *telegram_session_loop_chat_state_file_path;
    const char *telegram_session_loop_poll_seconds;
    const char *telegram_session_loop_max_iterations;
    const char *telegram_session_loop_default_offset_file_path;
    const char *telegram_session_loop_default_inbox_log_file_path;
    const char *telegram_session_loop_default_chat_state_file_path;
    const char *telegram_session_loop_default_poll_seconds;
    const char *telegram_session_loop_default_max_iterations;
    const char *telegram_manual_client_token_file_path;
    const char *telegram_manual_client_offset_file_path;
    const char *telegram_manual_client_inbox_log_file_path;
    const char *telegram_manual_client_chat_state_file_path;
    const char *telegram_manual_client_poll_seconds;
    const char *telegram_manual_client_max_iterations;
    const char *telegram_manual_client_default_offset_file_path;
    const char *telegram_manual_client_default_inbox_log_file_path;
    const char *telegram_manual_client_default_chat_state_file_path;
    const char *telegram_manual_client_default_poll_seconds;
    const char *telegram_manual_client_default_max_iterations;
    const char *telegram_client_token_file_path;
    const char *telegram_client_poll_seconds;
    const char *telegram_client_max_iterations;
    const char *telegram_client_default_poll_seconds;
    const char *telegram_client_default_max_iterations;
    const char *telegram_client_console_poll_seconds;
    const char *telegram_client_console_max_iterations;
    const char *telegram_human_chat_poll_seconds;
    const char *telegram_chats_file_path;
    const char *telegram_reply_default_index;
    const char *telegram_reply_default_text;
    const char *telegram_send_last_default_text;
    const char *telegram_echo_once_token_file_path;
    const char *telegram_echo_once_offset;
    const char *telegram_echo_once_default_offset;
    const char *telegram_echo_once_state_token_file_path;
    const char *telegram_echo_once_state_offset_file_path;
    const char *telegram_echo_once_state_default_offset_file_path;
    const char *telegram_echo_loop_token_file_path;
    const char *telegram_echo_loop_offset_file_path;
    const char *telegram_echo_loop_poll_seconds;
    const char *telegram_echo_loop_max_iterations;
    const char *telegram_echo_loop_default_offset_file_path;
    const char *telegram_echo_loop_default_poll_seconds;
    const char *telegram_echo_loop_default_max_iterations;
    const char *telegram_send_message_token_file_path;
    const char *telegram_send_message_chat_id;
    const char *telegram_send_message_text;
    const char *telegram_send_message_default_chat_id;
    const char *telegram_send_message_default_text;
    const char *telegram_send_chat_token_file_path;
    const char *telegram_send_chat_file_path;
    const char *telegram_send_chat_index;
    const char *telegram_send_chat_text;
    const char *telegram_send_chat_default_file_path;
    const char *telegram_send_chat_default_index;
    const char *telegram_send_chat_default_text;
    tg_log_level log_level;
    int tls_verify;
    int show_help;
    int run_net_test;
    int run_http_test;
    int run_http_post_self_test;
    int run_https_test;
    int run_platform_rng_test;
    int run_gui_self_test;
    int run_gui_window;
    int run_gui_chats;                   /* GUI window over a real peer cache */
    int run_gui_chats_live;              /* refresh the cache from the network first */
    int run_gui_live;                    /* live window: sidebar + notification ticks */
    int run_gui_live_debug;              /* same + crash-safe lifecycle log to disk */
    int run_gui_session_tick_self;       /* smoke: open a live session + a few ticks */
    const char *gui_chats_cache_file;    /* peer cache to project into the sidebar */
    int run_chat_engine_self_test;
    int run_chat_render_self_test;
    int run_chat_list_self_test;
    int run_gui_driver_self_test;
    int run_console_ui_test;
    int run_console_tui_test;
    const char *ui_color_mode;   /* 0 / "on" / "off" / "auto" */
    const char *ui_charset;      /* 0 / "latin1" / "utf8" */
    const char *ui_theme;        /* 0 / "dark" / "plain" */
    const char *ui_tui;          /* 0 / "on" / "off" */
    int run_mtproto_self_test;
    int run_mtproto_self_test_fast;
    int run_mtproto_self_test_heavy;
    int run_mtproto_2fa_bench;
    int run_mtproto_crypto_bench;
    int run_mtproto_req_pq_probe;
    int run_mtproto_req_dh_probe;
    int run_mtproto_auth_send_code;
    int run_mtproto_auth_send_code_file;
    int run_mtproto_auth_sign_in;
    int run_mtproto_auth_sign_in_file;
    int run_mtproto_auth_sign_up;
    int run_mtproto_auth_get_config;
    int run_mtproto_auth_get_config_file;
    int run_mtproto_auth_get_password;
    int run_mtproto_auth_get_password_file;
    int run_mtproto_auth_check_password;
    int run_mtproto_auth_check_password_file;
    int run_mtproto_auth_login_wizard_file;
    int run_mtproto_auth_status;
    int run_mtproto_auth_status_file;
    int run_mtproto_auth_inspect;
    int run_mtproto_auth_check_local_files;
    int run_mtproto_auth_get_self;
    int run_mtproto_auth_get_dialogs;
    int run_mtproto_auth_get_dialogs_file;
    int run_mtproto_auth_list_peers_file;
    int run_mtproto_auth_resolve_username_file;
    int run_mtproto_auth_get_history_self;
    int run_mtproto_auth_get_history_self_file;
    int run_mtproto_auth_get_history_peer_file;
    int run_mtproto_auth_send_self;
    int run_mtproto_auth_send_peer_file;
    int run_mtproto_chat_file;
    int run_mtproto_start_file;
    int run_mtproto_auth_forget;
    int run_telegram_tls_status;
    int run_json_test;
    int run_telegram_json_test;
    int run_telegram_json_self_test;
    int run_telegram_path_test;
    int run_telegram_http_self_test;
    int run_telegram_token_file_path_test;
    int run_telegram_default_token_file_path_test;
    int run_telegram_preflight;
    int run_telegram_get_me_self_test;
    int run_telegram_get_me;
    int run_telegram_get_me_default;
    int run_telegram_get_updates_self_test;
    int run_telegram_get_updates;
    int run_telegram_get_updates_default;
    int run_telegram_read_once_state_self_test;
    int run_telegram_read_once_state;
    int run_telegram_read_once_state_default;
    int run_telegram_read_loop;
    int run_telegram_read_loop_default;
    int run_telegram_inbox_self_test;
    int run_telegram_inbox;
    int run_telegram_inbox_default;
    int run_telegram_inbox_loop;
    int run_telegram_inbox_loop_default;
    int run_telegram_session;
    int run_telegram_session_default;
    int run_telegram_session_loop;
    int run_telegram_session_loop_default;
    int run_telegram_manual_client;
    int run_telegram_manual_client_default;
    int run_telegram_offset_state_self_test;
    int run_telegram_console_self_test;
    int run_telegram_client_state_self_test;
    int run_telegram_client_self_test;
    int run_telegram_text_client_self_test;
    int run_telegram_client;
    int run_telegram_client_default;
    int run_telegram_client_console;
    int run_telegram_human_chat;
    int run_telegram_chats;
    int run_telegram_chats_default;
    int run_telegram_reply_default;
    int run_telegram_send_last_default;
    int run_telegram_echo_once_self_test;
    int run_telegram_echo_once;
    int run_telegram_echo_once_default;
    int run_telegram_echo_once_state;
    int run_telegram_echo_once_state_default;
    int run_telegram_echo_loop;
    int run_telegram_echo_loop_default;
    int run_telegram_send_message_self_test;
    int run_telegram_send_message;
    int run_telegram_send_message_default;
    int run_telegram_send_chat;
    int run_telegram_send_chat_default;
} tg_config;

/**
 * Initializes config with platform defaults and all test flags disabled.
 */
void tg_config_init(tg_config *config);

/**
 * In-place upgrade cleanup: removes the pre-0.0.6 leftovers (telegram-test,
 * the IconX launcher scripts and their icons) from the program's own drawer
 * (PROGDIR: only, exact names). Call once at startup; silent no-op when
 * nothing is there or on the hosted build.
 */
void tg_config_remove_superseded(void);

/**
 * Parses command-line options into config.
 *
 * No memory is allocated. String options are stored as pointers to argv values.
 * Returns 0 on success and non-zero on invalid or incomplete arguments.
 */
int tg_config_parse(tg_config *config, int argc, char **argv);

/**
 * Prints supported command-line options to stream.
 */
void tg_config_print_usage(FILE *stream, const char *program_name);

#endif
