/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tg_config.h"
#include "tg_platform.h"

/* The data/ drawer holds every auxiliary file except telegram-auth.bin. The
   launchers create it and migrate old root files, but the binary must NOT
   depend on that: an updated binary under an old launcher (or a bare launch)
   still finds its internally-defaulted files by migrating them here. The
   peers file is deliberately NOT touched -- its path always arrives as a
   launcher argument, so wherever the launcher says, it stays. */
static void tg_config_move_to_data(const char *old_name, const char *new_name)
{
    FILE *f;

    f = fopen(old_name, "rb");
    if (f == 0) {
        return; /* nothing to migrate */
    }
    fclose(f);
    f = fopen(new_name, "rb");
    if (f != 0) {
        fclose(f); /* the data/ copy is the live one: drop the stale root
                      duplicate so the drawer stays tidy */
        (void)remove(old_name);
        return;
    }
    (void)rename(old_name, new_name); /* same volume: keeps the content */
}

static void tg_config_ensure_data_dir(void)
{
    (void)mkdir("data", 0777); /* EEXIST is the norm */
    tg_config_move_to_data("telegram-api.txt", "data/telegram-api.txt");
    tg_config_move_to_data("phone-code-hash.txt", "data/phone-code-hash.txt");
    tg_config_move_to_data("telegram-gui-win.txt",
                           "data/telegram-gui-win.txt");
}

/* In-place upgrade cleanup: copying a 0.0.6+ drawer over an old one leaves the
   pre-rename binary (telegram-test), the IconX launcher scripts and their
   icons behind, next to the new self-launch icons -- confusing duplicates on
   Workbench. Remove EXACT known names only, and only from the program's own
   drawer (PROGDIR:), never the CWD: a shell launch from elsewhere must not
   touch look-alike files. Session/data files are never in this list. On the
   hosted build PROGDIR: does not resolve and remove() fails silently. */
void tg_config_remove_superseded(void)
{
    static const char *victims[] = {
        "PROGDIR:telegram-test",
        "PROGDIR:TelegramGUI",
        "PROGDIR:TelegramGUI.info",
        "PROGDIR:TelegramTUI",
        "PROGDIR:TelegramTUI.info",
        "PROGDIR:RunMTProtoStart",
        "PROGDIR:RunMTProtoStart.info"
    };
    unsigned long i;

    for (i = 0UL; i < sizeof(victims) / sizeof(victims[0]); ++i) {
        (void)remove(victims[i]);
    }
}

/* Adopts a user/launcher-given auxiliary-file path into data/: only BARE
   filenames are touched (anything with ':' or '/' is an explicit location
   the caller chose -- respect it). The root file, when present, is migrated
   so old launchers converge on the tidy layout instead of breaking on it.
   Returns the path the caller should use; `slot` must outlive the config. */
static const char *tg_config_adopt_data_path(const char *given, char *slot,
                                             unsigned long slot_size)
{
    const char *p;

    if (given == 0 || given[0] == '\0') {
        return given;
    }
    for (p = given; *p != '\0'; ++p) {
        if (*p == ':' || *p == '/') {
            return given; /* explicit location: hands off */
        }
    }
    if (strlen(given) + 6UL >= slot_size) {
        return given;
    }
    tg_config_ensure_data_dir();
    strcpy(slot, "data/");
    strcat(slot, given);
    tg_config_move_to_data(given, slot);
    return slot;
}

void tg_config_init(tg_config *config)
{
    config->data_dir = tg_platform_default_data_dir();
    config->token_file_path_override = 0;
    config->connect_timeout_seconds = 0;
    config->tls_ca_file = 0;
    config->tls_ca_path = 0;
    config->inbox_log_file_path = 0;
    config->chat_state_file_path = 0;
    config->net_test_host = 0;
    config->net_test_port = 0;
    config->http_test_host = 0;
    config->http_test_port = 0;
    config->http_test_path = 0;
    config->https_test_host = 0;
    config->https_test_port = 0;
    config->https_test_path = 0;
    config->mtproto_probe_host = 0;
    config->mtproto_probe_port = 0;
    config->mtproto_probe_dc_id = 0;
    config->mtproto_auth_host = 0;
    config->mtproto_auth_port = 0;
    config->mtproto_auth_dc_id = 0;
    config->mtproto_auth_api_id = 0;
    config->mtproto_auth_api_hash = 0;
    config->mtproto_auth_api_file = 0;
    config->mtproto_auth_phone = 0;
    config->mtproto_auth_file = 0;
    config->mtproto_auth_code_hash_file = 0;
    config->mtproto_auth_code = 0;
    config->mtproto_auth_first_name = 0;
    config->mtproto_auth_last_name = 0;
    config->mtproto_auth_password_file = 0;
    config->mtproto_auth_limit = 0;
    config->mtproto_auth_message = 0;
    config->mtproto_auth_peer_cache_file = 0;
    config->mtproto_auth_peer_index = 0;
    config->mtproto_auth_username = 0;
    config->mtproto_chat_peer_cache_file = 0;
    config->json_test_input = 0;
    config->json_test_field = 0;
    config->telegram_json_test_input = 0;
    config->telegram_path_test_token = 0;
    config->telegram_path_test_method = 0;
    config->telegram_token_file_path = 0;
    config->telegram_token_file_method = 0;
    config->telegram_default_token_file_method = 0;
    config->telegram_get_me_token_file_path = 0;
    config->telegram_get_updates_token_file_path = 0;
    config->telegram_get_updates_offset = 0;
    config->telegram_get_updates_default_offset = 0;
    config->telegram_read_once_state_token_file_path = 0;
    config->telegram_read_once_state_offset_file_path = 0;
    config->telegram_read_once_state_default_offset_file_path = 0;
    config->telegram_read_loop_token_file_path = 0;
    config->telegram_read_loop_offset_file_path = 0;
    config->telegram_read_loop_poll_seconds = 0;
    config->telegram_read_loop_max_iterations = 0;
    config->telegram_read_loop_default_offset_file_path = 0;
    config->telegram_read_loop_default_poll_seconds = 0;
    config->telegram_read_loop_default_max_iterations = 0;
    config->telegram_inbox_token_file_path = 0;
    config->telegram_inbox_offset_file_path = 0;
    config->telegram_inbox_default_offset_file_path = 0;
    config->telegram_inbox_loop_token_file_path = 0;
    config->telegram_inbox_loop_offset_file_path = 0;
    config->telegram_inbox_loop_poll_seconds = 0;
    config->telegram_inbox_loop_max_iterations = 0;
    config->telegram_inbox_loop_default_offset_file_path = 0;
    config->telegram_inbox_loop_default_poll_seconds = 0;
    config->telegram_inbox_loop_default_max_iterations = 0;
    config->telegram_session_token_file_path = 0;
    config->telegram_session_offset_file_path = 0;
    config->telegram_session_inbox_log_file_path = 0;
    config->telegram_session_chat_state_file_path = 0;
    config->telegram_session_default_offset_file_path = 0;
    config->telegram_session_default_inbox_log_file_path = 0;
    config->telegram_session_default_chat_state_file_path = 0;
    config->telegram_session_loop_token_file_path = 0;
    config->telegram_session_loop_offset_file_path = 0;
    config->telegram_session_loop_inbox_log_file_path = 0;
    config->telegram_session_loop_chat_state_file_path = 0;
    config->telegram_session_loop_poll_seconds = 0;
    config->telegram_session_loop_max_iterations = 0;
    config->telegram_session_loop_default_offset_file_path = 0;
    config->telegram_session_loop_default_inbox_log_file_path = 0;
    config->telegram_session_loop_default_chat_state_file_path = 0;
    config->telegram_session_loop_default_poll_seconds = 0;
    config->telegram_session_loop_default_max_iterations = 0;
    config->telegram_manual_client_token_file_path = 0;
    config->telegram_manual_client_offset_file_path = 0;
    config->telegram_manual_client_inbox_log_file_path = 0;
    config->telegram_manual_client_chat_state_file_path = 0;
    config->telegram_manual_client_poll_seconds = 0;
    config->telegram_manual_client_max_iterations = 0;
    config->telegram_manual_client_default_offset_file_path = 0;
    config->telegram_manual_client_default_inbox_log_file_path = 0;
    config->telegram_manual_client_default_chat_state_file_path = 0;
    config->telegram_manual_client_default_poll_seconds = 0;
    config->telegram_manual_client_default_max_iterations = 0;
    config->telegram_client_token_file_path = 0;
    config->telegram_client_poll_seconds = 0;
    config->telegram_client_max_iterations = 0;
    config->telegram_client_default_poll_seconds = 0;
    config->telegram_client_default_max_iterations = 0;
    config->telegram_client_console_poll_seconds = 0;
    config->telegram_client_console_max_iterations = 0;
    config->telegram_human_chat_poll_seconds = 0;
    config->telegram_chats_file_path = 0;
    config->telegram_reply_default_index = 0;
    config->telegram_reply_default_text = 0;
    config->telegram_send_last_default_text = 0;
    config->telegram_echo_once_token_file_path = 0;
    config->telegram_echo_once_offset = 0;
    config->telegram_echo_once_default_offset = 0;
    config->telegram_echo_once_state_token_file_path = 0;
    config->telegram_echo_once_state_offset_file_path = 0;
    config->telegram_echo_once_state_default_offset_file_path = 0;
    config->telegram_echo_loop_token_file_path = 0;
    config->telegram_echo_loop_offset_file_path = 0;
    config->telegram_echo_loop_poll_seconds = 0;
    config->telegram_echo_loop_max_iterations = 0;
    config->telegram_echo_loop_default_offset_file_path = 0;
    config->telegram_echo_loop_default_poll_seconds = 0;
    config->telegram_echo_loop_default_max_iterations = 0;
    config->telegram_send_message_token_file_path = 0;
    config->telegram_send_message_chat_id = 0;
    config->telegram_send_message_text = 0;
    config->telegram_send_message_default_chat_id = 0;
    config->telegram_send_message_default_text = 0;
    config->telegram_send_chat_token_file_path = 0;
    config->telegram_send_chat_file_path = 0;
    config->telegram_send_chat_index = 0;
    config->telegram_send_chat_text = 0;
    config->telegram_send_chat_default_file_path = 0;
    config->telegram_send_chat_default_index = 0;
    config->telegram_send_chat_default_text = 0;
    config->log_level = TG_LOG_INFO;
    config->tls_verify = 0;
    config->show_help = 0;
    config->run_net_test = 0;
    config->run_http_test = 0;
    config->run_http_post_self_test = 0;
    config->run_https_test = 0;
    config->run_platform_rng_test = 0;
    config->run_gui_self_test = 0;
    config->run_gui_window = 0;
    config->run_gui_chats = 0;
    config->run_gui_chats_live = 0;
    config->run_gui_live = 0;
    config->run_gui_live_debug = 0;
    config->run_gui_session_tick_self = 0;
    config->gui_chats_cache_file = 0;
    config->run_chat_engine_self_test = 0;
    config->run_chat_render_self_test = 0;
    config->run_chat_list_self_test = 0;
    config->run_gui_driver_self_test = 0;
    config->run_console_ui_test = 0;
    config->run_console_tui_test = 0;
    config->ui_color_mode = 0;
    config->ui_charset = 0;
    config->ui_theme = 0;
    config->ui_tui = 0;
    config->run_mtproto_self_test = 0;
    config->run_mtproto_self_test_fast = 0;
    config->run_mtproto_self_test_heavy = 0;
    config->run_mtproto_2fa_bench = 0;
    config->run_mtproto_crypto_bench = 0;
    config->media_bench_dir = 0;
    config->run_mtproto_req_pq_probe = 0;
    config->run_mtproto_req_dh_probe = 0;
    config->run_mtproto_auth_send_code = 0;
    config->run_mtproto_auth_send_code_file = 0;
    config->run_mtproto_auth_sign_in = 0;
    config->run_mtproto_auth_sign_in_file = 0;
    config->run_mtproto_auth_sign_up = 0;
    config->run_mtproto_auth_get_config = 0;
    config->run_mtproto_auth_get_config_file = 0;
    config->run_mtproto_auth_get_password = 0;
    config->run_mtproto_auth_get_password_file = 0;
    config->run_mtproto_auth_check_password = 0;
    config->run_mtproto_auth_check_password_file = 0;
    config->run_mtproto_auth_login_wizard_file = 0;
    config->run_mtproto_auth_status = 0;
    config->run_mtproto_auth_status_file = 0;
    config->run_mtproto_auth_inspect = 0;
    config->run_mtproto_auth_check_local_files = 0;
    config->run_mtproto_auth_get_self = 0;
    config->run_mtproto_auth_get_dialogs = 0;
    config->run_mtproto_auth_get_dialogs_file = 0;
    config->run_mtproto_auth_list_peers_file = 0;
    config->run_mtproto_auth_resolve_username_file = 0;
    config->run_mtproto_auth_get_history_self = 0;
    config->run_mtproto_auth_get_history_self_file = 0;
    config->run_mtproto_auth_get_history_peer_file = 0;
    config->run_mtproto_auth_send_self = 0;
    config->run_mtproto_auth_send_peer_file = 0;
    config->run_mtproto_chat_file = 0;
    config->run_mtproto_start_file = 0;
    config->run_mtproto_auth_forget = 0;
    config->run_telegram_tls_status = 0;
    config->run_json_test = 0;
    config->run_telegram_json_test = 0;
    config->run_telegram_json_self_test = 0;
    config->run_telegram_path_test = 0;
    config->run_telegram_http_self_test = 0;
    config->run_telegram_token_file_path_test = 0;
    config->run_telegram_default_token_file_path_test = 0;
    config->run_telegram_preflight = 0;
    config->run_telegram_get_me_self_test = 0;
    config->run_telegram_get_me = 0;
    config->run_telegram_get_me_default = 0;
    config->run_telegram_get_updates_self_test = 0;
    config->run_telegram_get_updates = 0;
    config->run_telegram_get_updates_default = 0;
    config->run_telegram_read_once_state_self_test = 0;
    config->run_telegram_read_once_state = 0;
    config->run_telegram_read_once_state_default = 0;
    config->run_telegram_read_loop = 0;
    config->run_telegram_read_loop_default = 0;
    config->run_telegram_inbox_self_test = 0;
    config->run_telegram_inbox = 0;
    config->run_telegram_inbox_default = 0;
    config->run_telegram_inbox_loop = 0;
    config->run_telegram_inbox_loop_default = 0;
    config->run_telegram_session = 0;
    config->run_telegram_session_default = 0;
    config->run_telegram_session_loop = 0;
    config->run_telegram_session_loop_default = 0;
    config->run_telegram_manual_client = 0;
    config->run_telegram_manual_client_default = 0;
    config->run_telegram_offset_state_self_test = 0;
    config->run_telegram_console_self_test = 0;
    config->run_telegram_client_state_self_test = 0;
    config->run_telegram_client_self_test = 0;
    config->run_telegram_text_client_self_test = 0;
    config->run_telegram_client = 0;
    config->run_telegram_client_default = 0;
    config->run_telegram_client_console = 0;
    config->run_telegram_human_chat = 0;
    config->run_telegram_chats = 0;
    config->run_telegram_chats_default = 0;
    config->run_telegram_reply_default = 0;
    config->run_telegram_send_last_default = 0;
    config->run_telegram_echo_once_self_test = 0;
    config->run_telegram_echo_once = 0;
    config->run_telegram_echo_once_default = 0;
    config->run_telegram_echo_once_state = 0;
    config->run_telegram_echo_once_state_default = 0;
    config->run_telegram_echo_loop = 0;
    config->run_telegram_echo_loop_default = 0;
    config->run_telegram_send_message_self_test = 0;
    config->run_telegram_send_message = 0;
    config->run_telegram_send_message_default = 0;
    config->run_telegram_send_chat = 0;
    config->run_telegram_send_chat_default = 0;
}

int tg_config_parse(tg_config *config, int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            config->show_help = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            config->log_level = TG_LOG_DEBUG;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            config->log_level = TG_LOG_WARN;
        } else if (strcmp(argv[i], "--data-dir") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->data_dir = argv[i];
        } else if (strcmp(argv[i], "--token-file") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->token_file_path_override = argv[i];
        } else if (strcmp(argv[i], "--connect-timeout") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->connect_timeout_seconds = argv[i];
        } else if (strcmp(argv[i], "--tls-verify") == 0) {
            config->tls_verify = 1;
        } else if (strcmp(argv[i], "--tls-ca-file") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->tls_ca_file = argv[i];
        } else if (strcmp(argv[i], "--tls-ca-path") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->tls_ca_path = argv[i];
        } else if (strcmp(argv[i], "--inbox-log-file") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->inbox_log_file_path = argv[i];
        } else if (strcmp(argv[i], "--chat-state-file") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            ++i;
            config->chat_state_file_path = argv[i];
        } else if (strcmp(argv[i], "--net-test") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_net_test = 1;
            config->net_test_host = argv[i + 1];
            config->net_test_port = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--http-test") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_http_test = 1;
            config->http_test_host = argv[i + 1];
            config->http_test_port = argv[i + 2];
            config->http_test_path = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--http-post-self-test") == 0) {
            config->run_http_post_self_test = 1;
        } else if (strcmp(argv[i], "--https-test") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_https_test = 1;
            config->https_test_host = argv[i + 1];
            config->https_test_port = argv[i + 2];
            config->https_test_path = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--platform-rng-test") == 0) {
            config->run_platform_rng_test = 1;
        } else if (strcmp(argv[i], "--gui-self-test") == 0) {
            config->run_gui_self_test = 1;
        } else if (strcmp(argv[i], "--gui-test") == 0) {
            config->run_gui_window = 1;
        } else if (strcmp(argv[i], "--gui-chats") == 0) {
            config->run_gui_chats = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                {
                    static char peers_slot[96];

                    config->gui_chats_cache_file = tg_config_adopt_data_path(
                        argv[i + 1], peers_slot, sizeof(peers_slot));
                }
                i += 1;
            } else {
                config->gui_chats_cache_file = "data/telegram-peers.txt";
            }
        } else if (strcmp(argv[i], "--gui-live") == 0 ||
                   strcmp(argv[i], "--gui-live-debug") == 0) {
            config->run_gui_live = 1;
            if (strcmp(argv[i], "--gui-live-debug") == 0) {
                config->run_gui_live_debug = 1;
            }
            tg_config_ensure_data_dir();
            config->mtproto_auth_api_file = "data/telegram-api.txt";
            config->mtproto_auth_file = "telegram-auth.bin";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                {
                    static char peers_slot[96];

                    config->gui_chats_cache_file = tg_config_adopt_data_path(
                        argv[i + 1], peers_slot, sizeof(peers_slot));
                }
                i += 1;
            } else {
                config->gui_chats_cache_file = "data/telegram-peers.txt";
            }
        } else if (strcmp(argv[i], "--gui-session-tick-self") == 0) {
            config->run_gui_session_tick_self = 1;
            tg_config_ensure_data_dir();
            config->mtproto_auth_api_file = "data/telegram-api.txt";
            config->mtproto_auth_file = "telegram-auth.bin";
            config->gui_chats_cache_file = "data/telegram-peers.txt";
        } else if (strcmp(argv[i], "--gui-chats-live") == 0) {
            config->run_gui_chats = 1;
            config->run_gui_chats_live = 1;
            tg_config_ensure_data_dir();
            config->mtproto_auth_api_file = "data/telegram-api.txt";
            config->mtproto_auth_file = "telegram-auth.bin";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                {
                    static char peers_slot[96];

                    config->gui_chats_cache_file = tg_config_adopt_data_path(
                        argv[i + 1], peers_slot, sizeof(peers_slot));
                }
                i += 1;
            } else {
                config->gui_chats_cache_file = "data/telegram-peers.txt";
            }
        } else if (strcmp(argv[i], "--chat-engine-self-test") == 0) {
            config->run_chat_engine_self_test = 1;
        } else if (strcmp(argv[i], "--chat-render-self-test") == 0) {
            config->run_chat_render_self_test = 1;
        } else if (strcmp(argv[i], "--chat-list-self-test") == 0) {
            config->run_chat_list_self_test = 1;
        } else if (strcmp(argv[i], "--gui-driver-self-test") == 0) {
            config->run_gui_driver_self_test = 1;
        } else if (strcmp(argv[i], "--console-ui-test") == 0) {
            config->run_console_ui_test = 1;
        } else if (strcmp(argv[i], "--console-tui-test") == 0) {
            config->run_console_tui_test = 1;
        } else if (strcmp(argv[i], "--ui-color") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->ui_color_mode = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--ui-charset") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->ui_charset = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--ui-theme") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->ui_theme = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--ui-tui") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->ui_tui = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--mtproto-self-test") == 0) {
            config->run_mtproto_self_test = 1;
        } else if (strcmp(argv[i], "--mtproto-self-test-fast") == 0) {
            config->run_mtproto_self_test_fast = 1;
        } else if (strcmp(argv[i], "--mtproto-self-test-heavy") == 0) {
            config->run_mtproto_self_test_heavy = 1;
        } else if (strcmp(argv[i], "--mtproto-2fa-bench") == 0) {
            config->run_mtproto_2fa_bench = 1;
        } else if (strcmp(argv[i], "--mtproto-crypto-bench") == 0) {
            config->run_mtproto_crypto_bench = 1;
        } else if (strcmp(argv[i], "--media-bench") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->media_bench_dir = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--mtproto-req-pq-probe") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_mtproto_req_pq_probe = 1;
            config->mtproto_probe_host = argv[i + 1];
            config->mtproto_probe_port = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--mtproto-req-dh-probe") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_mtproto_req_dh_probe = 1;
            config->mtproto_probe_host = argv[i + 1];
            config->mtproto_probe_port = argv[i + 2];
            config->mtproto_probe_dc_id = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--mtproto-auth-send-code") == 0) {
            if (i + 8 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_send_code = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_dc_id = argv[i + 3];
            config->mtproto_auth_api_id = argv[i + 4];
            config->mtproto_auth_api_hash = argv[i + 5];
            config->mtproto_auth_phone = argv[i + 6];
            config->mtproto_auth_file = argv[i + 7];
            config->mtproto_auth_code_hash_file = argv[i + 8];
            i += 8;
        } else if (strcmp(argv[i], "--mtproto-auth-send-code-file") == 0) {
            if (i + 7 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_send_code_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_dc_id = argv[i + 3];
            config->mtproto_auth_api_file = argv[i + 4];
            config->mtproto_auth_phone = argv[i + 5];
            config->mtproto_auth_file = argv[i + 6];
            config->mtproto_auth_code_hash_file = argv[i + 7];
            i += 7;
        } else if (strcmp(argv[i], "--mtproto-auth-sign-in") == 0) {
            if (i + 8 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_sign_in = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_phone = argv[i + 5];
            config->mtproto_auth_code_hash_file = argv[i + 6];
            config->mtproto_auth_code = argv[i + 7];
            config->mtproto_auth_dc_id = argv[i + 8];
            i += 8;
        } else if (strcmp(argv[i], "--mtproto-auth-sign-in-file") == 0) {
            if (i + 8 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_sign_in_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_phone = argv[i + 5];
            config->mtproto_auth_code_hash_file = argv[i + 6];
            config->mtproto_auth_code = argv[i + 7];
            config->mtproto_auth_dc_id = argv[i + 8];
            i += 8;
        } else if (strcmp(argv[i], "--mtproto-auth-sign-up") == 0) {
            if (i + 9 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_sign_up = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_phone = argv[i + 5];
            config->mtproto_auth_code_hash_file = argv[i + 6];
            config->mtproto_auth_first_name = argv[i + 7];
            config->mtproto_auth_last_name = argv[i + 8];
            config->mtproto_auth_dc_id = argv[i + 9];
            i += 9;
        } else if (strcmp(argv[i], "--mtproto-auth-get-config") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_config = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-get-config-file") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_config_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-get-password") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_password = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-get-password-file") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_password_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-check-password") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_check_password = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_password_file = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-check-password-file") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_check_password_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_password_file = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-login-wizard-file") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_login_wizard_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_dc_id = argv[i + 3];
            config->mtproto_auth_api_file = argv[i + 4];
            config->mtproto_auth_file = argv[i + 5];
            config->mtproto_auth_code_hash_file = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-status") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_status = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-status-file") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_status_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-inspect") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_inspect = 1;
            config->mtproto_auth_file = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--mtproto-auth-check-local-files") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_check_local_files = 1;
            config->mtproto_auth_api_file = argv[i + 1];
            config->mtproto_auth_file = argv[i + 2];
            i += 2;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->mtproto_auth_password_file = argv[i + 1];
                ++i;
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->mtproto_auth_code_hash_file = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--mtproto-auth-get-self") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_self = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--mtproto-auth-get-dialogs") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_dialogs = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_limit = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-get-dialogs-file") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_dialogs_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_limit = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-list-peers-file") == 0) {
            if (i + 7 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_list_peers_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_limit = argv[i + 6];
            config->mtproto_auth_peer_cache_file = argv[i + 7];
            i += 7;
        } else if (strcmp(argv[i], "--mtproto-auth-resolve-username-file") == 0) {
            if (i + 7 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_resolve_username_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_username = argv[i + 6];
            config->mtproto_auth_peer_cache_file = argv[i + 7];
            i += 7;
        } else if (strcmp(argv[i], "--mtproto-auth-get-history-self") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_history_self = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_limit = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-get-history-self-file") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_history_self_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_limit = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-get-history-peer-file") == 0) {
            if (i + 8 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_get_history_peer_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_peer_cache_file = argv[i + 6];
            config->mtproto_auth_peer_index = argv[i + 7];
            config->mtproto_auth_limit = argv[i + 8];
            i += 8;
        } else if (strcmp(argv[i], "--mtproto-auth-send-self") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_send_self = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_id = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_message = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-auth-send-peer-file") == 0) {
            if (i + 8 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_send_peer_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_auth_peer_cache_file = argv[i + 6];
            config->mtproto_auth_peer_index = argv[i + 7];
            config->mtproto_auth_message = argv[i + 8];
            i += 8;
        } else if (strcmp(argv[i], "--mtproto-chat-file") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_mtproto_chat_file = 1;
            config->mtproto_auth_host = argv[i + 1];
            config->mtproto_auth_port = argv[i + 2];
            config->mtproto_auth_api_file = argv[i + 3];
            config->mtproto_auth_file = argv[i + 4];
            config->mtproto_auth_dc_id = argv[i + 5];
            config->mtproto_chat_peer_cache_file = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--mtproto-start") == 0) {
            config->run_mtproto_start_file = 1;
            tg_config_ensure_data_dir();
            config->mtproto_auth_api_file = "data/telegram-api.txt";
            config->mtproto_auth_file = "telegram-auth.bin";
            config->mtproto_auth_code_hash_file = "phone-code-hash.txt";
            config->mtproto_chat_peer_cache_file = "data/telegram-peers.txt";
        } else if (strcmp(argv[i], "--mtproto-start-file") == 0) {
            if (i + 4 >= argc) {
                return 1;
            }
            config->run_mtproto_start_file = 1;
            config->mtproto_auth_api_file = argv[i + 1];
            config->mtproto_auth_file = argv[i + 2];
            config->mtproto_auth_code_hash_file = argv[i + 3];
            {
                static char peers_slot_tui[96];

                config->mtproto_chat_peer_cache_file =
                    tg_config_adopt_data_path(argv[i + 4], peers_slot_tui,
                                              sizeof(peers_slot_tui));
            }
            i += 4;
        } else if (strcmp(argv[i], "--mtproto-auth-forget") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_mtproto_auth_forget = 1;
            config->mtproto_auth_file = argv[i + 1];
            ++i;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->mtproto_auth_code_hash_file = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-tls-status") == 0) {
            config->run_telegram_tls_status = 1;
        } else if (strcmp(argv[i], "--json-test") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_json_test = 1;
            config->json_test_input = argv[i + 1];
            config->json_test_field = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-json-test") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_json_test = 1;
            config->telegram_json_test_input = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-json-self-test") == 0) {
            config->run_telegram_json_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-path-test") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_path_test = 1;
            config->telegram_path_test_token = argv[i + 1];
            config->telegram_path_test_method = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-http-self-test") == 0) {
            config->run_telegram_http_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-token-file-path-test") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_token_file_path_test = 1;
            config->telegram_token_file_path = argv[i + 1];
            config->telegram_token_file_method = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-default-token-file-path-test") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_default_token_file_path_test = 1;
            config->telegram_default_token_file_method = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-preflight") == 0) {
            config->run_telegram_preflight = 1;
        } else if (strcmp(argv[i], "--telegram-getme-self-test") == 0) {
            config->run_telegram_get_me_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-getme") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_get_me = 1;
            config->telegram_get_me_token_file_path = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-getme-default") == 0) {
            config->run_telegram_get_me_default = 1;
        } else if (strcmp(argv[i], "--telegram-get-updates-self-test") == 0) {
            config->run_telegram_get_updates_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-get-updates") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_get_updates = 1;
            config->telegram_get_updates_token_file_path = argv[i + 1];
            ++i;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_get_updates_offset = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-get-updates-default") == 0) {
            config->run_telegram_get_updates_default = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_get_updates_default_offset = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-read-once-state-self-test") == 0) {
            config->run_telegram_read_once_state_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-read-once-state") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_read_once_state = 1;
            config->telegram_read_once_state_token_file_path = argv[i + 1];
            config->telegram_read_once_state_offset_file_path = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-read-once-state-default") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_read_once_state_default = 1;
            config->telegram_read_once_state_default_offset_file_path = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-read-loop") == 0) {
            if (i + 4 >= argc) {
                return 1;
            }
            config->run_telegram_read_loop = 1;
            config->telegram_read_loop_token_file_path = argv[i + 1];
            config->telegram_read_loop_offset_file_path = argv[i + 2];
            config->telegram_read_loop_poll_seconds = argv[i + 3];
            config->telegram_read_loop_max_iterations = argv[i + 4];
            i += 4;
        } else if (strcmp(argv[i], "--telegram-read-loop-default") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_telegram_read_loop_default = 1;
            config->telegram_read_loop_default_offset_file_path = argv[i + 1];
            config->telegram_read_loop_default_poll_seconds = argv[i + 2];
            config->telegram_read_loop_default_max_iterations = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--telegram-inbox-self-test") == 0) {
            config->run_telegram_inbox_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-inbox") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_inbox = 1;
            config->telegram_inbox_token_file_path = argv[i + 1];
            config->telegram_inbox_offset_file_path = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-inbox-default") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_inbox_default = 1;
            config->telegram_inbox_default_offset_file_path = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-inbox-loop") == 0) {
            if (i + 4 >= argc) {
                return 1;
            }
            config->run_telegram_inbox_loop = 1;
            config->telegram_inbox_loop_token_file_path = argv[i + 1];
            config->telegram_inbox_loop_offset_file_path = argv[i + 2];
            config->telegram_inbox_loop_poll_seconds = argv[i + 3];
            config->telegram_inbox_loop_max_iterations = argv[i + 4];
            i += 4;
        } else if (strcmp(argv[i], "--telegram-inbox-loop-default") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_telegram_inbox_loop_default = 1;
            config->telegram_inbox_loop_default_offset_file_path = argv[i + 1];
            config->telegram_inbox_loop_default_poll_seconds = argv[i + 2];
            config->telegram_inbox_loop_default_max_iterations = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--telegram-session") == 0) {
            if (i + 4 >= argc) {
                return 1;
            }
            config->run_telegram_session = 1;
            config->telegram_session_token_file_path = argv[i + 1];
            config->telegram_session_offset_file_path = argv[i + 2];
            config->telegram_session_inbox_log_file_path = argv[i + 3];
            config->telegram_session_chat_state_file_path = argv[i + 4];
            i += 4;
        } else if (strcmp(argv[i], "--telegram-session-default") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_telegram_session_default = 1;
            config->telegram_session_default_offset_file_path = argv[i + 1];
            config->telegram_session_default_inbox_log_file_path = argv[i + 2];
            config->telegram_session_default_chat_state_file_path = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--telegram-session-loop") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_telegram_session_loop = 1;
            config->telegram_session_loop_token_file_path = argv[i + 1];
            config->telegram_session_loop_offset_file_path = argv[i + 2];
            config->telegram_session_loop_inbox_log_file_path = argv[i + 3];
            config->telegram_session_loop_chat_state_file_path = argv[i + 4];
            config->telegram_session_loop_poll_seconds = argv[i + 5];
            config->telegram_session_loop_max_iterations = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--telegram-session-loop-default") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_telegram_session_loop_default = 1;
            config->telegram_session_loop_default_offset_file_path = argv[i + 1];
            config->telegram_session_loop_default_inbox_log_file_path = argv[i + 2];
            config->telegram_session_loop_default_chat_state_file_path = argv[i + 3];
            config->telegram_session_loop_default_poll_seconds = argv[i + 4];
            config->telegram_session_loop_default_max_iterations = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--telegram-manual-client") == 0) {
            if (i + 6 >= argc) {
                return 1;
            }
            config->run_telegram_manual_client = 1;
            config->telegram_manual_client_token_file_path = argv[i + 1];
            config->telegram_manual_client_offset_file_path = argv[i + 2];
            config->telegram_manual_client_inbox_log_file_path = argv[i + 3];
            config->telegram_manual_client_chat_state_file_path = argv[i + 4];
            config->telegram_manual_client_poll_seconds = argv[i + 5];
            config->telegram_manual_client_max_iterations = argv[i + 6];
            i += 6;
        } else if (strcmp(argv[i], "--telegram-manual-client-default") == 0) {
            if (i + 5 >= argc) {
                return 1;
            }
            config->run_telegram_manual_client_default = 1;
            config->telegram_manual_client_default_offset_file_path = argv[i + 1];
            config->telegram_manual_client_default_inbox_log_file_path = argv[i + 2];
            config->telegram_manual_client_default_chat_state_file_path = argv[i + 3];
            config->telegram_manual_client_default_poll_seconds = argv[i + 4];
            config->telegram_manual_client_default_max_iterations = argv[i + 5];
            i += 5;
        } else if (strcmp(argv[i], "--telegram-offset-state-self-test") == 0) {
            config->run_telegram_offset_state_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-console-self-test") == 0) {
            config->run_telegram_console_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-client-state-self-test") == 0) {
            config->run_telegram_client_state_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-client-self-test") == 0) {
            config->run_telegram_client_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-text-client-self-test") == 0) {
            config->run_telegram_text_client_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-client") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_client = 1;
            config->telegram_client_token_file_path = argv[i + 1];
            ++i;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_client_poll_seconds = argv[i + 1];
                ++i;
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_client_max_iterations = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-client-default") == 0) {
            config->run_telegram_client_default = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_client_default_poll_seconds = argv[i + 1];
                ++i;
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_client_default_max_iterations = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-client-console") == 0) {
            config->run_telegram_client_console = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_client_console_poll_seconds = argv[i + 1];
                ++i;
            }
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_client_console_max_iterations = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-human-chat") == 0 ||
                   strcmp(argv[i], "--telegram-chat") == 0) {
            config->run_telegram_human_chat = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_human_chat_poll_seconds = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-chats") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_chats = 1;
            config->telegram_chats_file_path = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-chats-default") == 0) {
            config->run_telegram_chats_default = 1;
        } else if (strcmp(argv[i], "--telegram-reply-default") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_reply_default = 1;
            config->telegram_reply_default_index = argv[i + 1];
            config->telegram_reply_default_text = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-echo-once-self-test") == 0) {
            config->run_telegram_echo_once_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-echo-once") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_echo_once = 1;
            config->telegram_echo_once_token_file_path = argv[i + 1];
            ++i;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_echo_once_offset = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-echo-once-default") == 0) {
            config->run_telegram_echo_once_default = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config->telegram_echo_once_default_offset = argv[i + 1];
                ++i;
            }
        } else if (strcmp(argv[i], "--telegram-echo-once-state") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_echo_once_state = 1;
            config->telegram_echo_once_state_token_file_path = argv[i + 1];
            config->telegram_echo_once_state_offset_file_path = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-echo-once-state-default") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_echo_once_state_default = 1;
            config->telegram_echo_once_state_default_offset_file_path = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--telegram-echo-loop") == 0) {
            if (i + 4 >= argc) {
                return 1;
            }
            config->run_telegram_echo_loop = 1;
            config->telegram_echo_loop_token_file_path = argv[i + 1];
            config->telegram_echo_loop_offset_file_path = argv[i + 2];
            config->telegram_echo_loop_poll_seconds = argv[i + 3];
            config->telegram_echo_loop_max_iterations = argv[i + 4];
            i += 4;
        } else if (strcmp(argv[i], "--telegram-echo-loop-default") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_telegram_echo_loop_default = 1;
            config->telegram_echo_loop_default_offset_file_path = argv[i + 1];
            config->telegram_echo_loop_default_poll_seconds = argv[i + 2];
            config->telegram_echo_loop_default_max_iterations = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--telegram-send-message-self-test") == 0) {
            config->run_telegram_send_message_self_test = 1;
        } else if (strcmp(argv[i], "--telegram-send-message") == 0 ||
                   strcmp(argv[i], "--telegram-send") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_telegram_send_message = 1;
            config->telegram_send_message_token_file_path = argv[i + 1];
            config->telegram_send_message_chat_id = argv[i + 2];
            config->telegram_send_message_text = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--telegram-send-message-default") == 0 ||
                   strcmp(argv[i], "--telegram-send-default") == 0) {
            if (i + 2 >= argc) {
                return 1;
            }
            config->run_telegram_send_message_default = 1;
            config->telegram_send_message_default_chat_id = argv[i + 1];
            config->telegram_send_message_default_text = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--telegram-send-chat") == 0) {
            if (i + 4 >= argc) {
                return 1;
            }
            config->run_telegram_send_chat = 1;
            config->telegram_send_chat_token_file_path = argv[i + 1];
            config->telegram_send_chat_file_path = argv[i + 2];
            config->telegram_send_chat_index = argv[i + 3];
            config->telegram_send_chat_text = argv[i + 4];
            i += 4;
        } else if (strcmp(argv[i], "--telegram-send-chat-default") == 0) {
            if (i + 3 >= argc) {
                return 1;
            }
            config->run_telegram_send_chat_default = 1;
            config->telegram_send_chat_default_file_path = argv[i + 1];
            config->telegram_send_chat_default_index = argv[i + 2];
            config->telegram_send_chat_default_text = argv[i + 3];
            i += 3;
        } else if (strcmp(argv[i], "--telegram-send-last-default") == 0) {
            if (i + 1 >= argc) {
                return 1;
            }
            config->run_telegram_send_last_default = 1;
            config->telegram_send_last_default_text = argv[i + 1];
            ++i;
        } else {
            return 1;
        }
    }

    return 0;
}

void tg_config_print_usage(FILE *stream, const char *program_name)
{
    fprintf(stream, "Usage: %s [options]\n", program_name);
    fprintf(stream, "\n");
    fprintf(stream, "Options:\n");
    fprintf(stream, "  -h, --help            Show this help\n");
    fprintf(stream, "  -v, --verbose         Enable debug logging\n");
    fprintf(stream, "  -q, --quiet           Show warnings and errors only\n");
    fprintf(stream, "      --data-dir <path> Set application data directory\n");
    fprintf(stream, "      --token-file <path>\n");
    fprintf(stream, "                         Override default Telegram token file\n");
    fprintf(stream, "      --connect-timeout <seconds>\n");
    fprintf(stream, "                         Limit TCP connect on supported platforms (0 disables)\n");
    fprintf(stream, "      --tls-verify       Verify TLS certificate chain and hostname\n");
    fprintf(stream, "      --tls-ca-file <path>\n");
    fprintf(stream, "                         CA bundle file for --tls-verify\n");
    fprintf(stream, "      --tls-ca-path <path>\n");
    fprintf(stream, "                         CA directory for --tls-verify\n");
    fprintf(stream, "      --inbox-log-file <path>\n");
    fprintf(stream, "                         Append read-only inbox items to a local text log\n");
    fprintf(stream, "      --chat-state-file <path>\n");
    fprintf(stream, "                         Update one-line-per-chat state during inbox reads\n");
    fprintf(stream, "      --net-test <host> <port>\n");
    fprintf(stream, "                         Test DNS resolution and TCP connect\n");
    fprintf(stream, "      --http-test <host> <port> <path>\n");
    fprintf(stream, "                         Test TCP send and receive with HTTP/1.0\n");
    fprintf(stream, "      --http-post-self-test\n");
    fprintf(stream, "                         Run built-in HTTP POST request builder sample\n");
    fprintf(stream, "      --https-test <host> <port> <path>\n");
    fprintf(stream, "                         Test TLS send and receive with HTTP/1.0\n");
    fprintf(stream, "      --platform-rng-test\n");
    fprintf(stream, "                         Check whether secure platform RNG is available\n");
    fprintf(stream, "      --console-ui-test\n");
    fprintf(stream, "                         Show colour roles, glyphs and emoji mapping\n");
    fprintf(stream, "      --console-tui-test\n");
    fprintf(stream, "                         Try the full-screen chat layout on this console\n");
    fprintf(stream, "      --gui-self-test\n");
    fprintf(stream, "                         Run the portable GUI layout self-test (host-runnable)\n");
    fprintf(stream, "      --gui-test\n");
    fprintf(stream, "                         Open the native demo GUI window (Amiga; host prints a notice)\n");
    fprintf(stream, "      --gui-chats [peer-cache-file]\n");
    fprintf(stream, "                         Open the GUI window over a real peer cache (default telegram-peers.txt);\n");
    fprintf(stream, "                         the sidebar shows your chats. F1-F10 select (Shift for 11-20), Q quits\n");
    fprintf(stream, "      --gui-chats-live [peer-cache-file]\n");
    fprintf(stream, "                         Like --gui-chats but first refreshes the cache from the network\n");
    fprintf(stream, "                         (telegram-api.txt + telegram-auth.bin), so the sidebar is live\n");
    fprintf(stream, "      --gui-live [peer-cache-file]\n");
    fprintf(stream, "                         Open the live GUI window: real sidebar + a background poll\n");
    fprintf(stream, "                         that flashes a chat's unread badge as messages arrive\n");
    fprintf(stream, "      --gui-live-debug [peer-cache-file]\n");
    fprintf(stream, "                         Like --gui-live, plus a crash-safe lifecycle log to\n");
    fprintf(stream, "                         tg-gui-debug.log (in the current dir) for diagnosing crashes\n");
    fprintf(stream, "      --gui-session-tick-self\n");
    fprintf(stream, "                         Smoke-test the live GUI session: open, run a few notification\n");
    fprintf(stream, "                         drains, close (needs telegram-api.txt + telegram-auth.bin)\n");
    fprintf(stream, "      --ui-color <on|off|auto>\n");
    fprintf(stream, "                         Console colours (default auto: on in chat)\n");
    fprintf(stream, "      --ui-charset <latin1|utf8>\n");
    fprintf(stream, "                         Console output charset (default latin1 on Amiga)\n");
    fprintf(stream, "      --ui-theme <dark|plain>\n");
    fprintf(stream, "                         Chat colour theme (default dark: black background)\n");
    fprintf(stream, "      --ui-tui <on|off>\n");
    fprintf(stream, "                         Full-screen chat layout (default on)\n");
    fprintf(stream, "      --mtproto-self-test\n");
    fprintf(stream, "                         Run offline MTProto bootstrap samples\n");
    fprintf(stream, "      --mtproto-self-test-fast\n");
    fprintf(stream, "                         Run offline MTProto samples excluding slow RSA/SRP checks\n");
    fprintf(stream, "      --mtproto-self-test-heavy\n");
    fprintf(stream, "                         Run slow offline MTProto RSA, bigint and SRP samples\n");
    fprintf(stream, "      --mtproto-req-pq-probe <host> <port>\n");
    fprintf(stream, "                         Send supervised MTProto req_pq_multi TCP probe\n");
    fprintf(stream, "      --mtproto-req-dh-probe <host> <port> <dc-id>\n");
    fprintf(stream, "                         Build auth key then send encrypted ping probe\n");
    fprintf(stream, "      --mtproto-auth-send-code <host> <port> <dc-id> <api-id> <api-hash> <phone> <auth-file> <code-hash-file>\n");
    fprintf(stream, "                         Build auth key, send auth.sendCode and save login state\n");
    fprintf(stream, "      --mtproto-auth-send-code-file <host> <port> <dc-id> <api-file> <phone> <auth-file> <code-hash-file>\n");
    fprintf(stream, "                         Send auth.sendCode with api_id/api_hash loaded from file\n");
    fprintf(stream, "      --mtproto-auth-sign-in <host> <port> <api-id> <auth-file> <phone> <code-hash-file> <code> <dc-id>\n");
    fprintf(stream, "                         Complete auth.signIn using saved login state\n");
    fprintf(stream, "      --mtproto-auth-sign-in-file <host> <port> <api-file> <auth-file> <phone> <code-hash-file> <code> <dc-id>\n");
    fprintf(stream, "                         Complete auth.signIn using local api file\n");
    fprintf(stream, "      --mtproto-auth-sign-up <host> <port> <api-id> <auth-file> <phone> <code-hash-file> <first-name> <last-name> <dc-id>\n");
    fprintf(stream, "                         Register a validated test phone after signup-required\n");
    fprintf(stream, "      --mtproto-auth-get-config <host> <port> <api-id> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Call help.getConfig with saved MTProto auth state\n");
    fprintf(stream, "      --mtproto-auth-get-config-file <host> <port> <api-file> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Call help.getConfig using local api file\n");
    fprintf(stream, "      --mtproto-auth-get-password <host> <port> <api-id> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Probe account.getPassword SRP metadata\n");
    fprintf(stream, "      --mtproto-auth-get-password-file <host> <port> <api-file> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Probe account.getPassword using local api file\n");
    fprintf(stream, "      --mtproto-auth-check-password <host> <port> <api-id> <auth-file> <dc-id> <password-file>\n");
    fprintf(stream, "                         Complete SRP 2FA login using a local password file\n");
    fprintf(stream, "      --mtproto-auth-check-password-file <host> <port> <api-file> <auth-file> <dc-id> <password-file>\n");
    fprintf(stream, "                         Complete SRP 2FA login using local api and password files\n");
    fprintf(stream, "      --mtproto-auth-login-wizard-file <host> <port> <dc-id> <api-file> <auth-file> <code-hash-file>\n");
    fprintf(stream, "                         Interactive MTProto phone/code/2FA login wizard\n");
    fprintf(stream, "      --mtproto-auth-status <host> <port> <api-id> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Check saved MTProto auth state without printing user data\n");
    fprintf(stream, "      --mtproto-auth-status-file <host> <port> <api-file> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Check saved MTProto auth state using local api file\n");
    fprintf(stream, "      --mtproto-auth-inspect <auth-file>\n");
    fprintf(stream, "                         Inspect a local MTProto auth file without printing secrets\n");
    fprintf(stream, "      --mtproto-auth-check-local-files <api-file> <auth-file> [password-file] [code-hash-file]\n");
    fprintf(stream, "                         Validate local MTProto login files before a real test\n");
    fprintf(stream, "      --mtproto-auth-get-self <host> <port> <api-id> <auth-file> <dc-id>\n");
    fprintf(stream, "                         Call users.getUsers(inputUserSelf) with saved auth state\n");
    fprintf(stream, "      --mtproto-auth-get-dialogs <host> <port> <api-id> <auth-file> <dc-id> <limit>\n");
    fprintf(stream, "                         Call messages.getDialogs with saved auth state\n");
    fprintf(stream, "      --mtproto-auth-get-dialogs-file <host> <port> <api-file> <auth-file> <dc-id> <limit>\n");
    fprintf(stream, "                         Call messages.getDialogs using local api file\n");
    fprintf(stream, "      --mtproto-auth-list-peers-file <host> <port> <api-file> <auth-file> <dc-id> <limit> <peer-cache-file>\n");
    fprintf(stream, "                         Save a local cache of dialog peer handles\n");
    fprintf(stream, "      --mtproto-auth-resolve-username-file <host> <port> <api-file> <auth-file> <dc-id> <username> <peer-cache-file>\n");
    fprintf(stream, "                         Add one public username to the local peer cache\n");
    fprintf(stream, "      --mtproto-auth-get-history-self <host> <port> <api-id> <auth-file> <dc-id> <limit>\n");
    fprintf(stream, "                         Call messages.getHistory for inputPeerSelf\n");
    fprintf(stream, "      --mtproto-auth-get-history-self-file <host> <port> <api-file> <auth-file> <dc-id> <limit>\n");
    fprintf(stream, "                         Call messages.getHistory(inputPeerSelf) using local api file\n");
    fprintf(stream, "      --mtproto-auth-get-history-peer-file <host> <port> <api-file> <auth-file> <dc-id> <peer-cache-file> <peer-index> <limit>\n");
    fprintf(stream, "                         Call messages.getHistory for a cached user peer\n");
    fprintf(stream, "      --mtproto-auth-send-self <host> <port> <api-id> <auth-file> <dc-id> <text>\n");
    fprintf(stream, "                         Send a text message to Saved Messages\n");
    fprintf(stream, "      --mtproto-auth-send-peer-file <host> <port> <api-file> <auth-file> <dc-id> <peer-cache-file> <peer-index> <text>\n");
    fprintf(stream, "                         Send a text message to a cached user peer\n");
    fprintf(stream, "      --mtproto-chat-file <host> <port> <api-file> <auth-file> <dc-id> <peer-cache-file>\n");
    fprintf(stream, "                         Interactive cached-user MTProto text chat\n");
    fprintf(stream, "      --mtproto-start\n");
    fprintf(stream, "                         Login if needed, then start MTProto text chat using default local files\n");
    fprintf(stream, "      --mtproto-start-file <api-file> <auth-file> <code-hash-file> <peer-cache-file>\n");
    fprintf(stream, "                         Login if needed, then start MTProto text chat using explicit local files\n");
    fprintf(stream, "      --mtproto-auth-forget <auth-file> [code-hash-file]\n");
    fprintf(stream, "                         Delete local MTProto auth test files\n");
    fprintf(stream, "                         Test DC ids may be passed as 10000+dc or test:<dc>\n");
    fprintf(stream, "      --telegram-tls-status\n");
    fprintf(stream, "                         Print current TLS security status\n");
    fprintf(stream, "      --json-test <json> <field>\n");
    fprintf(stream, "                         Test top-level JSON field lookup\n");
    fprintf(stream, "      --telegram-json-test <json>\n");
    fprintf(stream, "                         Test Telegram API response parsing\n");
    fprintf(stream, "      --telegram-json-self-test\n");
    fprintf(stream, "                         Run built-in Telegram JSON parser samples\n");
    fprintf(stream, "      --telegram-path-test <token> <method>\n");
    fprintf(stream, "                         Test Telegram Bot API path construction\n");
    fprintf(stream, "      --telegram-http-self-test\n");
    fprintf(stream, "                         Run built-in HTTP-to-Telegram parser samples\n");
    fprintf(stream, "      --telegram-token-file-path-test <file> <method>\n");
    fprintf(stream, "                         Load token file and test Bot API path construction\n");
    fprintf(stream, "      --telegram-default-token-file-path-test <method>\n");
    fprintf(stream, "                         Load default token file and test Bot API path construction\n");
    fprintf(stream, "      --telegram-preflight\n");
    fprintf(stream, "                         Check token path and Telegram HTTPS reachability\n");
    fprintf(stream, "      --telegram-getme-self-test\n");
    fprintf(stream, "                         Run built-in Bot API getMe parser sample\n");
    fprintf(stream, "      --telegram-getme <file>\n");
    fprintf(stream, "                         Call Telegram getMe with token loaded from file\n");
    fprintf(stream, "      --telegram-getme-default\n");
    fprintf(stream, "                         Call Telegram getMe with default token file\n");
    fprintf(stream, "      --telegram-get-updates-self-test\n");
    fprintf(stream, "                         Run built-in Bot API getUpdates parser sample\n");
    fprintf(stream, "      --telegram-get-updates <file> [offset]\n");
    fprintf(stream, "                         Call Telegram getUpdates with optional offset\n");
    fprintf(stream, "      --telegram-get-updates-default [offset]\n");
    fprintf(stream, "                         Call Telegram getUpdates with default token file\n");
    fprintf(stream, "      --telegram-read-once-state-self-test\n");
    fprintf(stream, "                         Run built-in read-only stateful update sample\n");
    fprintf(stream, "      --telegram-read-once-state <file> <offset-file>\n");
    fprintf(stream, "                         Read pending updates and save a persistent offset\n");
    fprintf(stream, "      --telegram-read-once-state-default <offset-file>\n");
    fprintf(stream, "                         Stateful read pending updates with default token file\n");
    fprintf(stream, "      --telegram-read-loop <file> <offset-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded stateful read polling\n");
    fprintf(stream, "      --telegram-read-loop-default <offset-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded read polling with default token file\n");
    fprintf(stream, "      --telegram-inbox-self-test\n");
    fprintf(stream, "                         Run built-in inbox-format update sample\n");
    fprintf(stream, "      --telegram-inbox <file> <offset-file>\n");
    fprintf(stream, "                         Print pending updates in inbox format and save offset\n");
    fprintf(stream, "      --telegram-inbox-default <offset-file>\n");
    fprintf(stream, "                         Inbox read using the default token file\n");
    fprintf(stream, "      --telegram-inbox-loop <file> <offset-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded inbox polling\n");
    fprintf(stream, "      --telegram-inbox-loop-default <offset-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded inbox polling with default token file\n");
    fprintf(stream, "      --telegram-session <file> <offset-file> <inbox-log> <chats-file>\n");
    fprintf(stream, "                         Run one manual-client receive session\n");
    fprintf(stream, "      --telegram-session-default <offset-file> <inbox-log> <chats-file>\n");
    fprintf(stream, "                         Manual-client receive session with default token file\n");
    fprintf(stream, "      --telegram-session-loop <file> <offset-file> <inbox-log> <chats-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded manual-client receive polling\n");
    fprintf(stream, "      --telegram-session-loop-default <offset-file> <inbox-log> <chats-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Bounded manual-client polling with default token file\n");
    fprintf(stream, "      --telegram-manual-client <file> <offset-file> <inbox-log> <chats-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Poll read-only updates, then list saved chats\n");
    fprintf(stream, "      --telegram-manual-client-default <offset-file> <inbox-log> <chats-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Manual-client preview with default token file\n");
    fprintf(stream, "      --telegram-offset-state-self-test\n");
    fprintf(stream, "                         Run built-in offset persistence sample\n");
    fprintf(stream, "      --telegram-console-self-test\n");
    fprintf(stream, "                         Run built-in interactive console parser sample\n");
    fprintf(stream, "      --telegram-client-state-self-test\n");
    fprintf(stream, "                         Run built-in saved-chat state sample\n");
    fprintf(stream, "      --telegram-client-self-test\n");
    fprintf(stream, "                         Run built-in simplified client state sample\n");
    fprintf(stream, "      --telegram-text-client-self-test\n");
    fprintf(stream, "                         Run built-in text-client command/state sample\n");
    fprintf(stream, "      --telegram-client <file> [poll-seconds] [max-iterations]\n");
    fprintf(stream, "                         Manual-client preview using default local state files\n");
    fprintf(stream, "      --telegram-client-default [poll-seconds] [max-iterations]\n");
    fprintf(stream, "                         Short manual-client preview with default token and state files\n");
    fprintf(stream, "      --telegram-client-console [poll-seconds] [max-iterations]\n");
    fprintf(stream, "                         Interactive manual console using default files\n");
    fprintf(stream, "      --telegram-human-chat [poll-seconds]\n");
    fprintf(stream, "                         Minimal human chat mode using default files\n");
    fprintf(stream, "      --telegram-chats <chats-file>\n");
    fprintf(stream, "                         List chats saved by manual-client sessions\n");
    fprintf(stream, "      --telegram-chats-default\n");
    fprintf(stream, "                         List chats from the default chat state file\n");
    fprintf(stream, "      --telegram-reply-default <index> <text>\n");
    fprintf(stream, "                         Send to a saved chat index using default files\n");
    fprintf(stream, "      --telegram-echo-once-self-test\n");
    fprintf(stream, "                         Run built-in one-shot echo flow sample\n");
    fprintf(stream, "      --telegram-echo-once <file> [offset]\n");
    fprintf(stream, "                         Read one update and echo its text back\n");
    fprintf(stream, "      --telegram-echo-once-default [offset]\n");
    fprintf(stream, "                         Echo one update using the default token file\n");
    fprintf(stream, "      --telegram-echo-once-state <file> <offset-file>\n");
    fprintf(stream, "                         Echo pending updates using a persistent offset file\n");
    fprintf(stream, "      --telegram-echo-once-state-default <offset-file>\n");
    fprintf(stream, "                         Stateful echo pending updates with default token file\n");
    fprintf(stream, "      --telegram-echo-loop <file> <offset-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded stateful echo polling\n");
    fprintf(stream, "      --telegram-echo-loop-default <offset-file> <poll-seconds> <max-iterations>\n");
    fprintf(stream, "                         Run bounded echo polling with default token file\n");
    fprintf(stream, "      --telegram-send-message-self-test\n");
    fprintf(stream, "                         Run built-in Bot API sendMessage parser sample\n");
    fprintf(stream, "      --telegram-send-message <file> <chat-id> <text>\n");
    fprintf(stream, "                         Send a Telegram message with token loaded from file\n");
    fprintf(stream, "      --telegram-send-message-default <chat-id> <text>\n");
    fprintf(stream, "                         Send a Telegram message with default token file\n");
    fprintf(stream, "      --telegram-send <file> <chat-id> <text>\n");
    fprintf(stream, "                         Alias for --telegram-send-message\n");
    fprintf(stream, "      --telegram-send-default <chat-id> <text>\n");
    fprintf(stream, "                         Alias for --telegram-send-message-default\n");
    fprintf(stream, "      --telegram-send-chat <file> <chats-file> <index> <text>\n");
    fprintf(stream, "                         Send to a saved chat by 1-based list index\n");
    fprintf(stream, "      --telegram-send-chat-default <chats-file> <index> <text>\n");
    fprintf(stream, "                         Send to a saved chat index with default token file\n");
    fprintf(stream, "      --telegram-send-last-default <text>\n");
    fprintf(stream, "                         Send to chat index 1 from the default chat state file\n");
}
