/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tg_app.h"
#include "tg_bot.h"
#include "tg_client_state.h"
#include "tg_config.h"
#include "tg_console.h"
#include "tg_console_ui.h"
#include "tg_chat_engine.h"
#include "tg_console_tui.h"
#include "tg_file.h"
#include "tg_gui.h"
#include "tg_version.h"
#include "tg_gui_driver.h"
#include "tg_gui_session.h"
#include "tg_https.h"
#include "tg_http.h"
#include "tg_json.h"
#include "tg_log.h"
#include "tg_mtproto.h"
#include "tg_mtproto_probe.h"
#include "tg_mtproto_session.h"
#include "tg_net.h"
#include "tg_offset_state.h"
#include "tg_platform.h"
#include "tg_telegram.h"
#include "tg_text_client.h"
#include "tg_tls.h"

#define TG_STATE_MAX_UPDATES 5UL

typedef enum tg_read_output_mode {
    TG_READ_OUTPUT_STATE = 0,
    TG_READ_OUTPUT_INBOX = 1,
    TG_READ_OUTPUT_CHAT = 2,
    TG_READ_OUTPUT_HUMAN = 3
} tg_read_output_mode;

static const char tg_default_token_file_name[] = "telegram-token.txt";
static const char tg_default_offset_file_name[] = "telegram-offset.txt";
static const char tg_default_inbox_log_file_name[] = "telegram-inbox.log";
static const char tg_default_chat_state_file_name[] = "telegram-chats.txt";
static const char tg_default_selected_chat_file_name[] = "telegram-selected-chat.txt";
static const char tg_default_poll_seconds_text[] = "5";
static const char tg_default_max_iterations_text[] = "10";
static const char tg_console_poll_seconds_text[] = "0";
static const char tg_console_max_iterations_text[] = "1";

static int tg_mtproto_production_endpoint_for_dc(unsigned long dc_id,
                                                 const char **host,
                                                 const char **dc_id_text)
{
    if (host == 0 || dc_id_text == 0) {
        return 1;
    }
    switch (dc_id) {
    case 1UL:
        *host = "149.154.175.50";
        *dc_id_text = "1";
        return 0;
    case 2UL:
        *host = "149.154.167.50";
        *dc_id_text = "2";
        return 0;
    case 3UL:
        *host = "149.154.175.100";
        *dc_id_text = "3";
        return 0;
    case 4UL:
        *host = "149.154.167.91";
        *dc_id_text = "4";
        return 0;
    case 5UL:
        *host = "91.108.56.130";
        *dc_id_text = "5";
        return 0;
    default:
        return 1;
    }
}

static void tg_app_trim_line(char *line)
{
    unsigned long length;

    if (line == 0) {
        return;
    }
    length = (unsigned long)strlen(line);
    while (length > 0UL &&
           (line[length - 1UL] == '\n' || line[length - 1UL] == '\r')) {
        line[length - 1UL] = '\0';
        --length;
    }
}

static int tg_app_prompt_required_line(const char *prompt,
                                       char *line,
                                       unsigned long line_size)
{
    if (prompt == 0 || line == 0 || line_size == 0UL) {
        return 2;
    }
    fputs(prompt, stdout);
    fflush(stdout);
    if (fgets(line, (int)line_size, stdin) == 0) {
        puts("mtproto setup: input-closed");
        return 2;
    }
    tg_app_trim_line(line);
    if (line[0] == '\0') {
        puts("mtproto setup: input-empty");
        return 2;
    }
    return 0;
}

static int tg_run_mtproto_ensure_api_file(const char *api_file)
{
    char api_probe[128];
    char api_id[32];
    char api_hash[96];
    char api_text[160];
    unsigned long api_probe_length;
    unsigned long api_id_length;
    unsigned long api_hash_length;
    unsigned long api_text_length;
    tg_file_status api_status;

    api_status = tg_file_read_text(api_file, api_probe, sizeof(api_probe),
                                   &api_probe_length);
    if (api_status == TG_FILE_OK) {
        return 0;
    }
    if (api_status != TG_FILE_OPEN_FAILED) {
        printf("Login setup required: cannot read %s (%s).\n", api_file,
               tg_file_status_name(api_status));
        return 2;
    }

    printf("Login setup: %s was not found.\n", api_file);
    puts("TelegramAmiga will create it now.");
    puts("Enter the Telegram API credentials for this client.");
    if (tg_app_prompt_required_line("api_id: ", api_id,
                                    sizeof(api_id)) != 0 ||
        tg_app_prompt_required_line("api_hash: ", api_hash,
                                    sizeof(api_hash)) != 0) {
        return 2;
    }

    api_id_length = (unsigned long)strlen(api_id);
    api_hash_length = (unsigned long)strlen(api_hash);
    api_text_length = api_id_length + 1UL + api_hash_length + 1UL;
    if (api_text_length >= sizeof(api_text)) {
        puts("mtproto setup: api credentials too long");
        return 2;
    }
    strcpy(api_text, api_id);
    api_text[api_id_length] = '\n';
    strcpy(api_text + api_id_length + 1UL, api_hash);
    api_text[api_text_length - 1UL] = '\n';
    api_text[api_text_length] = '\0';

    api_status = tg_file_write_text(api_file, api_text, api_text_length);
    if (api_status != TG_FILE_OK) {
        printf("mtproto setup: api-file-save-failed (%s)\n",
               tg_file_status_name(api_status));
        return 2;
    }
    printf("Saved %s.\n", api_file);
    return 0;
}

static int tg_run_mtproto_peer_cache_exists(const char *peer_cache_file)
{
    char probe[128];
    unsigned long length;

    if (peer_cache_file == 0 || peer_cache_file[0] == '\0') {
        return 0;
    }
    if (tg_file_read_text(peer_cache_file, probe, sizeof(probe), &length) !=
        TG_FILE_OK) {
        return 0;
    }
    return length > 0UL;
}

static int tg_run_mtproto_start_file(const char *api_file,
                                     const char *auth_file,
                                     const char *code_hash_file,
                                     const char *peer_cache_file)
{
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    tg_mtproto_session session;
    tg_mtproto_session_status session_status;
    const char *host;
    const char *dc_id_text;
    int rc;

    if (api_file == 0 || auth_file == 0 || code_hash_file == 0 ||
        peer_cache_file == 0) {
        puts("mtproto start: invalid-arguments");
        return 2;
    }
    if (tg_net_connect_timeout_seconds() == 0UL) {
        tg_net_set_connect_timeout_seconds(30UL);
    }

#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: tui start, checking api file");
#endif
    if (tg_run_mtproto_ensure_api_file(api_file) != 0) {
#ifdef TG_DIAG_TRACE
        tg_gui_log("diag: api file MISSING/unusable");
#endif
        return 2;
    }
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: loading saved session");
#endif

    session_status = tg_mtproto_session_load_authorization(auth_file,
                                                           &session,
                                                           auth_key);
    memset(auth_key, 0, sizeof(auth_key));
    if (session_status == TG_MTPROTO_SESSION_FILE_ERROR ||
        session_status == TG_MTPROTO_SESSION_PARSE_ERROR) {
        if (session_status == TG_MTPROTO_SESSION_FILE_ERROR) {
            puts("No saved login found. Starting login wizard.");
        } else {
            puts("Saved login is not usable. Starting login wizard.");
        }
#if defined(TG_LOWMEM)
        /* Plain-68000 build: the first login runs a Diffie-Hellman exchange,
           by far the heaviest thing this program does, and this profile
           reserves half the usual stack. Rather than let it grind or die,
           spell out the supported route before trying anything. */
        {
            char answer[16];

            puts("");
            puts("--------------------------------------------------------");
            puts("FIRST LOGIN ON A 68000: PLEASE READ");
            puts("");
            puts("Signing in for the first time needs a Diffie-Hellman key");
            puts("exchange. On a plain 68000 that takes a very long time and");
            puts("may run out of stack. Do it once elsewhere instead:");
            puts("");
            puts(" 1. Run Telegram Amiga on a faster Amiga (68020 or better)");
            puts("    or in an emulator (WinUAE, FS-UAE, vAmiga) using the");
            puts("    standard package, and log in there.");
            puts(" 2. Copy telegram-auth.bin from that drawer into THIS");
            puts("    drawer, next to TelegramAmiga. Copying");
            puts("    data/telegram-peers.txt too brings your chat list.");
            puts(" 3. Start this program again: it reuses that login and");
            puts("    goes straight to the chats.");
            puts("");
            puts("The same account works from both machines at once.");
            puts("");
            puts("Press RETURN to attempt the login here anyway,");
            puts("or Ctrl+C to quit and follow the steps above.");
            puts("--------------------------------------------------------");
            fflush(stdout);
            if (fgets(answer, (int)sizeof(answer), stdin) == 0) {
                return 2; /* console closed: nothing sensible left to do */
            }
        }
#endif
#ifdef TG_DIAG_TRACE
        tg_gui_log("diag: entering login wizard (network + DH)");
#endif
        rc = tg_mtproto_auth_login_wizard_file(
            "149.154.167.91", "443", "4", api_file, auth_file,
            code_hash_file, stdout);
#ifdef TG_DIAG_TRACE
        tg_gui_log(rc == 0 ? "diag: login wizard OK"
                           : "diag: login wizard FAILED");
#endif
        if (rc != 0) {
            return rc;
        }
        session_status = tg_mtproto_session_load_authorization(auth_file,
                                                               &session,
                                                               auth_key);
        memset(auth_key, 0, sizeof(auth_key));
    }
    if (session_status != TG_MTPROTO_SESSION_OK) {
#ifdef TG_DIAG_TRACE
        /* A600 hunt: a fast clean exit right after "loading saved session"
           was unexplainable from the log alone -- name the reason. */
        tg_gui_log("diag: auth file unusable, exiting");
#endif
        printf("mtproto start: auth-file-unusable %s\n",
               tg_mtproto_session_status_name(session_status));
        return 2;
    }
    if (tg_mtproto_production_endpoint_for_dc(session.dc_id, &host,
                                             &dc_id_text) != 0) {
#ifdef TG_DIAG_TRACE
        tg_gui_log("diag: unsupported dc, exiting");
#endif
        printf("mtproto start: unsupported-dc %lu\n", session.dc_id);
        return 2;
    }
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: session loaded, dc endpoint ok");
#endif

    if (!tg_run_mtproto_peer_cache_exists(peer_cache_file)) {
        puts("Login complete.");
        puts("No cached chat list found.");
        puts("Use /add name inside TelegramAmiga to search users or groups.");
    }

    puts("Opening chat.");
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: entering chat loop");
#endif
    return tg_mtproto_auth_chat_file(host, "443", api_file, auth_file,
                                     dc_id_text, peer_cache_file, stdout);
}

static int tg_run_platform_rng_test(void)
{
    unsigned char sample[32];
    unsigned long i;
    int all_zero;

    if (!tg_platform_random_bytes(sample, sizeof(sample))) {
        puts("secure rng: unavailable");
        return 2;
    }

    all_zero = 1;
    for (i = 0; i < sizeof(sample); ++i) {
        if (sample[i] != 0U) {
            all_zero = 0;
            break;
        }
    }
    memset(sample, 0, sizeof(sample));
    if (all_zero) {
        puts("secure rng: unavailable");
        return 2;
    }

    puts("secure rng: available");
    return 0;
}

static int tg_run_http_test(const tg_config *config)
{
    tg_http_status http_status;
    tg_http_parse_status parse_status;
    tg_http_response parsed_response;
    tg_net_status net_status;
    char net_error[128];
    char response[2048];
    unsigned long response_len;

    http_status = tg_http_get(config->http_test_host, config->http_test_port,
                              config->http_test_path, response, sizeof(response),
                              &response_len, &net_status, net_error, sizeof(net_error));
    if (http_status != TG_HTTP_OK) {
        printf("http test: failed: %s", tg_http_status_name(http_status));
        if (http_status == TG_HTTP_NET_ERROR) {
            printf(" / %s", tg_net_status_name(net_status));
        }
        if (net_error[0] != '\0') {
            printf(" (%s)", net_error);
        }
        printf("\n");
        return 2;
    }

    printf("http test: %s:%s%s ok, received %lu bytes\n",
           config->http_test_host, config->http_test_port,
           config->http_test_path, response_len);

    parse_status = tg_http_parse_response(response, response_len, &parsed_response);
    if (parse_status != TG_HTTP_PARSE_OK) {
        printf("http parse: failed: %s\n", tg_http_parse_status_name(parse_status));
        printf("%s\n", response);
        return 0;
    }

    printf("http status: %d", parsed_response.status_code);
    if (parsed_response.reason_length > 0) {
        printf(" %.*s", (int)parsed_response.reason_length, parsed_response.reason);
    }
    printf("\n");
    printf("http body: %lu bytes\n", parsed_response.body_length);
    printf("%.*s\n", (int)parsed_response.body_length, parsed_response.body);
    return 0;
}

static int tg_run_https_test(const tg_config *config)
{
    tg_https_status https_status;
    tg_http_parse_status parse_status;
    tg_http_response parsed_response;
    tg_tls_status tls_status;
    tg_net_status net_status;
    char net_error[128];
    char response[2048];
    unsigned long response_len;

    https_status = tg_https_get(config->https_test_host, config->https_test_port,
                                config->https_test_path, response, sizeof(response),
                                &response_len, &tls_status, &net_status,
                                net_error, sizeof(net_error));
    if (https_status != TG_HTTPS_OK) {
        printf("https test: failed: %s", tg_https_status_name(https_status));
        if (https_status == TG_HTTPS_TLS_ERROR) {
            printf(" / %s", tg_tls_status_name(tls_status));
            if (tls_status == TG_TLS_NET_ERROR) {
                printf(" / %s", tg_net_status_name(net_status));
            }
        }
        if (net_error[0] != '\0') {
            printf(" (%s)", net_error);
        }
        printf("\n");
        return 2;
    }

    printf("https test: %s:%s%s ok, received %lu bytes\n",
           config->https_test_host, config->https_test_port,
           config->https_test_path, response_len);

    parse_status = tg_http_parse_response(response, response_len, &parsed_response);
    if (parse_status != TG_HTTP_PARSE_OK) {
        printf("https parse: failed: %s\n", tg_http_parse_status_name(parse_status));
        printf("%s\n", response);
        return 0;
    }

    printf("https status: %d", parsed_response.status_code);
    if (parsed_response.reason_length > 0) {
        printf(" %.*s", (int)parsed_response.reason_length, parsed_response.reason);
    }
    printf("\n");
    printf("https body: %lu bytes\n", parsed_response.body_length);
    printf("%.*s\n", (int)parsed_response.body_length, parsed_response.body);
    return 0;
}

static int tg_run_telegram_tls_status(void)
{
    puts("telegram tls status:");
    printf("platform: %s\n", tg_platform_name());
    puts("transport encryption: platform TLS backend when available");
    puts("server name indication: enabled by supported TLS backends");
    printf("certificate validation requested: %s\n",
           tg_tls_certificate_validation_enabled() ? "yes" : "no");
    if (tg_tls_certificate_ca_file() != 0) {
        printf("certificate ca file: %s\n", tg_tls_certificate_ca_file());
    }
    if (tg_tls_certificate_ca_path() != 0) {
        printf("certificate ca path: %s\n", tg_tls_certificate_ca_path());
    }
    if (tg_tls_certificate_validation_enabled()) {
        puts("hostname verification: requested");
        puts("security note: validation support depends on the platform TLS backend");
    } else {
        puts("certificate validation plan: use --tls-verify with a CA file or CA path");
        puts("security note: use only test bots and disposable tokens without validation");
    }
    return 0;
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_http_post_self_test(void)
{
    static const char body[] = "{\"chat_id\":123,\"text\":\"Hello from Amiga\"}";
    char request[512];
    char length_header[64];
    unsigned long request_length;
    unsigned long body_length;
    tg_http_status http_status;

    body_length = (unsigned long)strlen(body);
    http_status = tg_http_build_post_request("api.telegram.org",
                                             "/botTOKEN/sendMessage",
                                             "application/json",
                                             body, body_length,
                                             request, sizeof(request),
                                             &request_length);
    if (http_status != TG_HTTP_OK) {
        printf("http post self-test: failed: %s\n", tg_http_status_name(http_status));
        return 2;
    }

    sprintf(length_header, "Content-Length: %lu\r\n", body_length);
    if (strncmp(request, "POST /botTOKEN/sendMessage HTTP/1.0\r\n",
                strlen("POST /botTOKEN/sendMessage HTTP/1.0\r\n")) != 0 ||
        strstr(request, "Host: api.telegram.org\r\n") == 0 ||
        strstr(request, "Content-Type: application/json\r\n") == 0 ||
        strstr(request, length_header) == 0 ||
        request_length < body_length ||
        strcmp(request + request_length - body_length, body) != 0) {
        puts("http post self-test: failed: request mismatch");
        printf("%s\n", request);
        return 2;
    }

    printf("http post self-test: ok request, %lu bytes\n", request_length);
    printf("http post body: %s\n", request + request_length - body_length);
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_json_test(const tg_config *config)
{
    tg_json_status json_status;
    tg_json_value value;
    unsigned long json_length;

    json_length = (unsigned long)strlen(config->json_test_input);
    json_status = tg_json_object_get(config->json_test_input, json_length,
                                     config->json_test_field, &value);
    if (json_status != TG_JSON_OK) {
        printf("json test: failed: %s\n", tg_json_status_name(json_status));
        return 2;
    }

    printf("json field: %s\n", config->json_test_field);
    printf("json type: %s\n", tg_json_value_type_name(value.type));

    if (value.type == TG_JSON_VALUE_BOOL) {
        printf("json value: %s\n", value.bool_value ? "true" : "false");
    } else if (value.type == TG_JSON_VALUE_STRING ||
               value.type == TG_JSON_VALUE_NUMBER ||
               value.type == TG_JSON_VALUE_OBJECT ||
               value.type == TG_JSON_VALUE_ARRAY) {
        printf("json value: %.*s\n", (int)value.length, value.start);
    } else {
        printf("json value: null\n");
    }

    return 0;
}

static void tg_print_telegram_response(const tg_telegram_response *response)
{
    printf("telegram ok: %s\n", response->ok ? "true" : "false");
    if (response->has_description) {
        printf("telegram description: %.*s\n",
               (int)response->description_length, response->description);
    }
    if (response->has_result) {
        printf("telegram result type: %s\n",
               tg_json_value_type_name(response->result.type));
        if (response->result.type != TG_JSON_VALUE_NULL &&
            response->result.start != 0) {
            printf("telegram result: %.*s\n",
                   (int)response->result.length, response->result.start);
        }
    }
}

static void tg_print_update_summary(const tg_bot_update_summary *update)
{
    tg_json_status json_status;
    char decoded_text[512];
    unsigned long decoded_length;

    if (!update->has_update) {
        puts("telegram update: none");
        return;
    }

    printf("telegram update id: %s\n", update->update_id);
    if (!update->has_message) {
        puts("telegram update message: none");
        return;
    }

    printf("telegram update chat id: %s\n", update->chat_id);
    if (update->has_text) {
        json_status = tg_json_string_decode(update->text, update->text_length,
                                            decoded_text, sizeof(decoded_text),
                                            &decoded_length);
        if (json_status == TG_JSON_OK) {
            printf("telegram update text: %s\n", decoded_text);
        } else {
            printf("telegram update text: <decode failed: %s>\n",
                   tg_json_status_name(json_status));
        }
    } else {
        puts("telegram update text: none");
    }
}

static int tg_app_append_char(char *buffer, unsigned long buffer_size,
                              unsigned long *position, char c)
{
    if (*position + 1 >= buffer_size) {
        return 1;
    }
    buffer[*position] = c;
    ++(*position);
    buffer[*position] = '\0';
    return 0;
}

static int tg_app_append_text(char *buffer, unsigned long buffer_size,
                              unsigned long *position, const char *text)
{
    while (*text != '\0') {
        if (tg_app_append_char(buffer, buffer_size, position, *text) != 0) {
            return 1;
        }
        ++text;
    }
    return 0;
}

static int tg_is_leap_year(unsigned long year)
{
    return (year % 4UL == 0UL) &&
           ((year % 100UL != 0UL) || (year % 400UL == 0UL));
}

static unsigned long tg_days_in_year(unsigned long year)
{
    return tg_is_leap_year(year) ? 366UL : 365UL;
}

static unsigned long tg_days_in_month(unsigned long year, unsigned long month)
{
    static const unsigned long month_days[] = {
        31UL, 28UL, 31UL, 30UL, 31UL, 30UL,
        31UL, 31UL, 30UL, 31UL, 30UL, 31UL
    };

    if (month == 2UL && tg_is_leap_year(year)) {
        return 29UL;
    }
    return month_days[month - 1UL];
}

static int tg_format_unix_utc(unsigned long timestamp,
                              char *buffer, unsigned long buffer_size)
{
    unsigned long seconds;
    unsigned long minutes;
    unsigned long hours;
    unsigned long days;
    unsigned long year;
    unsigned long month;
    unsigned long month_days;

    seconds = timestamp % 60UL;
    timestamp /= 60UL;
    minutes = timestamp % 60UL;
    timestamp /= 60UL;
    hours = timestamp % 24UL;
    days = timestamp / 24UL;

    year = 1970UL;
    while (days >= tg_days_in_year(year)) {
        days -= tg_days_in_year(year);
        ++year;
    }

    month = 1UL;
    for (;;) {
        month_days = tg_days_in_month(year, month);
        if (days < month_days) {
            break;
        }
        days -= month_days;
        ++month;
    }

    if (buffer_size < 24UL) {
        return 1;
    }
    sprintf(buffer, "%04lu-%02lu-%02lu %02lu:%02lu:%02lu UTC",
            year, month, days + 1UL, hours, minutes, seconds);
    return 0;
}

static int tg_format_inbox_date(const tg_bot_update_summary *update,
                                char *buffer, unsigned long buffer_size)
{
    unsigned long timestamp;
    char *end_ptr;
    unsigned long position;

    if (buffer == 0 || buffer_size == 0) {
        return 1;
    }
    buffer[0] = '\0';
    if (update == 0 || !update->has_date || update->date[0] == '\0') {
        position = 0;
        return tg_app_append_text(buffer, buffer_size, &position, "unknown");
    }

    end_ptr = 0;
    timestamp = strtoul(update->date, &end_ptr, 10);
    if (end_ptr == update->date || *end_ptr != '\0') {
        position = 0;
        return tg_app_append_text(buffer, buffer_size, &position, update->date);
    }

    if (tg_format_unix_utc(timestamp, buffer, buffer_size) != 0) {
        position = 0;
        return tg_app_append_text(buffer, buffer_size, &position, update->date);
    }

    return 0;
}

static void tg_sanitize_one_line(char *text)
{
    while (*text != '\0') {
        if (*text == '\r' || *text == '\n' || *text == '\t') {
            *text = ' ';
        }
        ++text;
    }
}

static int tg_inbox_content_text(const tg_bot_update_summary *update,
                                 char *buffer, unsigned long buffer_size)
{
    tg_json_status json_status;
    unsigned long decoded_length;
    unsigned long position;

    if (buffer == 0 || buffer_size == 0 || update == 0) {
        return 1;
    }
    buffer[0] = '\0';

    if (update->has_text) {
        json_status = tg_json_string_decode(update->text, update->text_length,
                                            buffer, buffer_size,
                                            &decoded_length);
        if (json_status != TG_JSON_OK) {
            position = 0;
            return tg_app_append_text(buffer, buffer_size, &position,
                                      "<decode failed>");
        }
        tg_sanitize_one_line(buffer);
        return 0;
    }

    position = 0;
    if (update->message_kind == TG_BOT_MESSAGE_PHOTO) {
        return tg_app_append_text(buffer, buffer_size, &position, "<photo>");
    } else if (update->message_kind == TG_BOT_MESSAGE_STICKER) {
        return tg_app_append_text(buffer, buffer_size, &position, "<sticker>");
    } else if (update->message_kind == TG_BOT_MESSAGE_DOCUMENT) {
        return tg_app_append_text(buffer, buffer_size, &position, "<document>");
    } else if (update->message_kind == TG_BOT_MESSAGE_UNSUPPORTED) {
        return tg_app_append_text(buffer, buffer_size, &position,
                                  "<unsupported message>");
    }

    return tg_app_append_text(buffer, buffer_size, &position, "<no message>");
}

static int tg_build_inbox_line(const tg_bot_update_summary *update,
                               char *buffer, unsigned long buffer_size)
{
    char date_text[64];
    char content_text[512];
    const char *sender;
    unsigned long position;

    if (update == 0 || buffer == 0 || buffer_size == 0 || !update->has_update) {
        return 1;
    }

    if (tg_format_inbox_date(update, date_text, sizeof(date_text)) != 0 ||
        tg_inbox_content_text(update, content_text, sizeof(content_text)) != 0) {
        return 1;
    }
    sender = update->has_sender_name ? update->sender_name : "unknown";

    position = 0;
    buffer[0] = '\0';
    if (tg_app_append_text(buffer, buffer_size, &position, date_text) != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, " | ") != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, update->update_id) != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, " | chat ") != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, update->chat_id) != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, " | ") != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, sender) != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, " | ") != 0 ||
        tg_app_append_text(buffer, buffer_size, &position,
                           tg_bot_message_kind_name(update->message_kind)) != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, " | ") != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, content_text) != 0) {
        return 1;
    }

    return 0;
}

static int tg_build_chat_display_line(const tg_bot_update_summary *update,
                                      char *buffer,
                                      unsigned long buffer_size)
{
    char content_text[512];
    const char *sender;
    unsigned long position;

    if (update == 0 || buffer == 0 || buffer_size == 0 || !update->has_update) {
        return 1;
    }

    if (!update->has_message) {
        position = 0;
        buffer[0] = '\0';
        return tg_app_append_text(buffer, buffer_size, &position,
                                  "telegram: <update without message>");
    }

    if (tg_inbox_content_text(update, content_text, sizeof(content_text)) != 0) {
        return 1;
    }
    sender = update->has_sender_name ? update->sender_name : update->chat_id;

    position = 0;
    buffer[0] = '\0';
    if (tg_app_append_text(buffer, buffer_size, &position, sender) != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, ": ") != 0 ||
        tg_app_append_text(buffer, buffer_size, &position, content_text) != 0) {
        return 1;
    }

    return 0;
}

static void tg_print_chat_update_summary(const tg_bot_update_summary *update)
{
    char line[768];

    if (update == 0 || !update->has_update) {
        puts("chat: no new messages");
        return;
    }
    if (tg_build_chat_display_line(update, line, sizeof(line)) != 0) {
        puts("chat: <message too long>");
        return;
    }
    puts(line);
}

static int tg_append_inbox_log_line_mode(const char *path,
                                         const tg_bot_update_summary *update,
                                         int verbose)
{
    tg_file_status file_status;
    char line[1024];
    unsigned long position;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    if (tg_build_inbox_line(update, line, sizeof(line)) != 0) {
        puts("inbox log: line too long");
        return 2;
    }

    position = (unsigned long)strlen(line);
    if (tg_app_append_char(line, sizeof(line), &position, '\n') != 0) {
        puts("inbox log: line too long");
        return 2;
    }

    file_status = tg_file_append_text(path, line, position);
    if (file_status != TG_FILE_OK) {
        printf("inbox log: write failed: %s\n",
               tg_file_status_name(file_status));
        return 2;
    }

    if (verbose) {
        printf("inbox log appended: %s\n", path);
    }
    return 0;
}

static int tg_append_inbox_log_line(const char *path,
                                    const tg_bot_update_summary *update)
{
    return tg_append_inbox_log_line_mode(path, update, 1);
}

static int tg_build_chat_state_line(const tg_bot_update_summary *update,
                                    char *buffer, unsigned long buffer_size)
{
    char date_text[64];
    char content_text[512];
    const char *sender_text;

    if (update == 0 || buffer == 0 || buffer_size == 0 ||
        !update->has_update || !update->has_message) {
        return 1;
    }

    if (tg_format_inbox_date(update, date_text, sizeof(date_text)) != 0 ||
        tg_inbox_content_text(update, content_text, sizeof(content_text)) != 0) {
        return 1;
    }
    if (update->has_sender_name) {
        sender_text = update->sender_name;
    } else {
        sender_text = "unknown";
    }

    return tg_client_state_build_line(update->chat_id,
                                      sender_text,
                                      date_text,
                                      content_text,
                                      buffer,
                                      buffer_size);
}

static int tg_update_chat_state_file_mode(const char *path,
                                          const tg_bot_update_summary *update,
                                          int verbose)
{
    char new_line[1024];

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    if (update == 0 || !update->has_message) {
        return 0;
    }
    if (tg_build_chat_state_line(update, new_line, sizeof(new_line)) != 0) {
        puts("chat state: line too long");
        return 2;
    }

    return tg_client_state_update_file_line(path, update->chat_id,
                                            new_line, verbose);
}

static int tg_update_chat_state_file(const char *path,
                                     const tg_bot_update_summary *update)
{
    return tg_update_chat_state_file_mode(path, update, 1);
}

static int tg_print_chat_line_callback(unsigned long index,
                                       const char *line,
                                       unsigned long line_length,
                                       void *context)
{
    char chat_id[64];
    char sender[128];
    char date[64];
    char text[512];

    (void)context;
    if (tg_client_state_parse_line(line, line_length,
                                   chat_id, sizeof(chat_id),
                                   sender, sizeof(sender),
                                   date, sizeof(date),
                                   text, sizeof(text)) != 0) {
        printf("%lu | <invalid chat state line>\n", index);
        return 0;
    }

    printf("%lu | chat %s | %s | %s | last: %s\n",
           index, chat_id, sender, date, text);
    return 0;
}

static void tg_print_inbox_update_summary(const tg_bot_update_summary *update)
{
    tg_json_status json_status;
    char decoded_text[512];
    char date_text[64];
    char line[1024];
    unsigned long decoded_length;

    if (!update->has_update) {
        puts("inbox: none");
        return;
    }

    printf("inbox update: %s\n", update->update_id);
    if (!update->has_message) {
        puts("inbox message: none");
        return;
    }

    if (tg_format_inbox_date(update, date_text, sizeof(date_text)) == 0) {
        printf("inbox date: %s\n", date_text);
    }
    printf("inbox chat: %s\n", update->chat_id);
    if (update->has_sender_name) {
        printf("inbox sender: %s\n", update->sender_name);
    } else {
        puts("inbox sender: unknown");
    }
    printf("inbox type: %s\n", tg_bot_message_kind_name(update->message_kind));
    if (update->has_text) {
        json_status = tg_json_string_decode(update->text, update->text_length,
                                            decoded_text, sizeof(decoded_text),
                                            &decoded_length);
        if (json_status == TG_JSON_OK) {
            printf("inbox text: %s\n", decoded_text);
        } else {
            printf("inbox text: <decode failed: %s>\n",
                   tg_json_status_name(json_status));
        }
    } else {
        if (update->message_kind == TG_BOT_MESSAGE_PHOTO) {
            puts("inbox text: <photo>");
        } else if (update->message_kind == TG_BOT_MESSAGE_STICKER) {
            puts("inbox text: <sticker>");
        } else if (update->message_kind == TG_BOT_MESSAGE_DOCUMENT) {
            puts("inbox text: <document>");
        } else {
            puts("inbox text: <non-text or unsupported message>");
        }
    }
    if (tg_build_inbox_line(update, line, sizeof(line)) == 0) {
        printf("inbox line: %s\n", line);
    }
}

static int tg_build_echo_text(const tg_bot_update_summary *update,
                              char *buffer, unsigned long buffer_size)
{
    static const char prefix[] = "Echo: ";
    unsigned long prefix_length;
    unsigned long decoded_length;
    tg_json_status json_status;

    if (buffer == 0 || buffer_size == 0 || update == 0 ||
        !update->has_text || update->text == 0) {
        return 1;
    }

    prefix_length = (unsigned long)strlen(prefix);
    if (prefix_length + 1 > buffer_size) {
        return 1;
    }

    strcpy(buffer, prefix);
    json_status = tg_json_string_decode(update->text, update->text_length,
                                        buffer + prefix_length,
                                        buffer_size - prefix_length,
                                        &decoded_length);
    if (json_status != TG_JSON_OK) {
        return 1;
    }
    return 0;
}

static int tg_is_decimal_text(const char *text)
{
    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return 0;
        }
        ++text;
    }
    return 1;
}

static int tg_parse_decimal_ulong(const char *text, unsigned long *value)
{
    unsigned long result;
    unsigned long digit;

    if (value == 0 || !tg_is_decimal_text(text)) {
        return 1;
    }

    result = 0;
    while (*text != '\0') {
        digit = (unsigned long)(*text - '0');
        if (result > (ULONG_MAX - digit) / 10UL) {
            return 1;
        }
        result = result * 10UL + digit;
        ++text;
    }

    *value = result;
    return 0;
}

static int tg_build_data_file_path(const char *data_dir, const char *file_name,
                                   char *buffer, unsigned long buffer_size)
{
    unsigned long data_dir_length;
    unsigned long file_name_length;
    unsigned long separator_length;
    char last_char;

    if (file_name == 0 || file_name[0] == '\0' ||
        buffer == 0 || buffer_size == 0) {
        return 1;
    }

    if (data_dir == 0) {
        data_dir = "";
    }

    data_dir_length = (unsigned long)strlen(data_dir);
    file_name_length = (unsigned long)strlen(file_name);
    separator_length = 0;

    if (data_dir_length > 0) {
        last_char = data_dir[data_dir_length - 1];
        if (last_char != ':' && last_char != '/' && last_char != '\\') {
            separator_length = 1;
        }
    }

    if (data_dir_length + separator_length + file_name_length + 1 > buffer_size) {
        return 1;
    }

    if (data_dir_length > 0) {
        memcpy(buffer, data_dir, data_dir_length);
    }
    if (separator_length > 0) {
        buffer[data_dir_length] = '/';
    }
    memcpy(buffer + data_dir_length + separator_length,
           file_name, file_name_length);
    buffer[data_dir_length + separator_length + file_name_length] = '\0';
    return 0;
}

static const char *tg_default_token_file_path(const tg_config *config,
                                             char *buffer,
                                             unsigned long buffer_size)
{
    if (config->token_file_path_override != 0 &&
        config->token_file_path_override[0] != '\0') {
        return config->token_file_path_override;
    }

    if (tg_build_data_file_path(config->data_dir, tg_default_token_file_name,
                                buffer, buffer_size) != 0) {
        puts("telegram token file: default path too long");
        return 0;
    }

    return buffer;
}

static const char *tg_default_named_file_path(const tg_config *config,
                                             const char *file_name,
                                             const char *label,
                                             char *buffer,
                                             unsigned long buffer_size)
{
    if (tg_build_data_file_path(config->data_dir, file_name, buffer,
                                buffer_size) != 0) {
        printf("telegram %s file: default path too long\n", label);
        return 0;
    }

    return buffer;
}

static int tg_run_telegram_json_test_text(const char *json)
{
    tg_telegram_status telegram_status;
    tg_telegram_response response;
    tg_json_status json_status;

    telegram_status = tg_telegram_parse_response(json, (unsigned long)strlen(json),
                                                 &response, &json_status);
    if (telegram_status != TG_TELEGRAM_OK) {
        printf("telegram json test: failed: %s", tg_telegram_status_name(telegram_status));
        if (telegram_status == TG_TELEGRAM_JSON_ERROR ||
            telegram_status == TG_TELEGRAM_MISSING_OK) {
            printf(" / %s", tg_json_status_name(json_status));
        }
        printf("\n");
        return 2;
    }

    tg_print_telegram_response(&response);
    return 0;
}

static int tg_run_telegram_json_test(const tg_config *config)
{
    return tg_run_telegram_json_test_text(config->telegram_json_test_input);
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_json_self_test(void)
{
    static const char ok_sample[] = "{\"ok\":true,\"result\":{\"id\":123,\"is_bot\":true}}";
    static const char error_sample[] = "{\"ok\":false,\"description\":\"Unauthorized\"}";

    puts("telegram json self-test: ok sample");
    if (tg_run_telegram_json_test_text(ok_sample) != 0) {
        return 2;
    }

    puts("telegram json self-test: error sample");
    if (tg_run_telegram_json_test_text(error_sample) != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_telegram_path_test(const tg_config *config)
{
    tg_telegram_status telegram_status;
    char path[256];
    unsigned long path_length;

    telegram_status = tg_telegram_build_bot_path(config->telegram_path_test_token,
                                                 config->telegram_path_test_method,
                                                 path, sizeof(path), &path_length);
    if (telegram_status != TG_TELEGRAM_OK) {
        printf("telegram path test: failed: %s\n",
               tg_telegram_status_name(telegram_status));
        return 2;
    }

    printf("telegram host: %s\n", tg_telegram_api_host());
    printf("telegram path: %s\n", path);
    printf("telegram path length: %lu\n", path_length);
    return 0;
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_http_test_text(const char *http_response)
{
    tg_telegram_status telegram_status;
    tg_telegram_http_response response;
    tg_http_parse_status http_parse_status;
    tg_json_status json_status;

    telegram_status = tg_telegram_parse_http_response(http_response,
                                                      (unsigned long)strlen(http_response),
                                                      &response, &http_parse_status,
                                                      &json_status);
    if (telegram_status != TG_TELEGRAM_OK) {
        printf("telegram http test: failed: %s", tg_telegram_status_name(telegram_status));
        if (telegram_status == TG_TELEGRAM_HTTP_PARSE_ERROR) {
            printf(" / %s", tg_http_parse_status_name(http_parse_status));
        } else if (telegram_status == TG_TELEGRAM_JSON_ERROR ||
                   telegram_status == TG_TELEGRAM_MISSING_OK) {
            printf(" / %s", tg_json_status_name(json_status));
        }
        printf("\n");
        return 2;
    }

    printf("telegram http status: %d\n", response.http_status_code);
    tg_print_telegram_response(&response.api);
    return 0;
}

static int tg_run_telegram_http_self_test(void)
{
    static const char ok_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":{\"id\":123,\"is_bot\":true}}";
    static const char error_response[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":false,\"description\":\"Unauthorized\"}";

    puts("telegram http self-test: ok response");
    if (tg_run_telegram_http_test_text(ok_response) != 0) {
        return 2;
    }

    puts("telegram http self-test: error response");
    if (tg_run_telegram_http_test_text(error_response) != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_telegram_token_file_path_test(const tg_config *config)
{
    tg_telegram_status telegram_status;
    tg_file_status file_status;
    char token[128];
    char path[256];
    unsigned long token_length;
    unsigned long path_length;

    telegram_status = tg_telegram_load_token_file(config->telegram_token_file_path,
                                                  token, sizeof(token),
                                                  &token_length, &file_status);
    if (telegram_status != TG_TELEGRAM_OK) {
        printf("telegram token file test: failed: %s", tg_telegram_status_name(telegram_status));
        if (telegram_status == TG_TELEGRAM_FILE_ERROR) {
            printf(" / %s", tg_file_status_name(file_status));
        }
        printf("\n");
        return 2;
    }

    telegram_status = tg_telegram_build_bot_path(token,
                                                 config->telegram_token_file_method,
                                                 path, sizeof(path), &path_length);
    if (telegram_status != TG_TELEGRAM_OK) {
        printf("telegram token file path test: failed: %s\n",
               tg_telegram_status_name(telegram_status));
        return 2;
    }

    printf("telegram host: %s\n", tg_telegram_api_host());
    printf("telegram method: %s\n", config->telegram_token_file_method);
    printf("telegram token length: %lu\n", token_length);
    printf("telegram path length: %lu\n", path_length);
    printf("telegram path: <redacted>\n");
    return 0;
}

static int tg_run_telegram_default_token_file_path_test(const tg_config *config)
{
    tg_config local_config;
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    local_config = *config;
    local_config.telegram_token_file_path = resolved_path;
    local_config.telegram_token_file_method =
        config->telegram_default_token_file_method;
    return tg_run_telegram_token_file_path_test(&local_config);
}

static int tg_run_telegram_preflight(const tg_config *config)
{
    tg_telegram_status telegram_status;
    tg_file_status file_status;
    tg_https_status https_status;
    tg_tls_status tls_status;
    tg_net_status net_status;
    tg_http_parse_status parse_status;
    tg_http_response parsed_response;
    char token_path[256];
    char token[128];
    char net_error[128];
    char response[4096];
    const char *resolved_path;
    unsigned long token_length;
    unsigned long response_length;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram preflight token file: %s\n", resolved_path);
    telegram_status = tg_telegram_load_token_file(resolved_path, token,
                                                  sizeof(token), &token_length,
                                                  &file_status);
    memset(token, 0, sizeof(token));
    if (telegram_status == TG_TELEGRAM_OK) {
        printf("telegram preflight token file: present, %lu bytes\n",
               token_length);
    } else if (telegram_status == TG_TELEGRAM_FILE_ERROR &&
               file_status == TG_FILE_OPEN_FAILED) {
        puts("telegram preflight token file: missing");
    } else {
        printf("telegram preflight token file: failed: %s",
               tg_telegram_status_name(telegram_status));
        if (telegram_status == TG_TELEGRAM_FILE_ERROR) {
            printf(" / %s", tg_file_status_name(file_status));
        }
        printf("\n");
        return 2;
    }

    net_error[0] = '\0';
    https_status = tg_https_get(tg_telegram_api_host(), "443", "/",
                                response, sizeof(response), &response_length,
                                &tls_status, &net_status, net_error,
                                sizeof(net_error));
    if (https_status != TG_HTTPS_OK) {
        printf("telegram preflight https: failed: %s",
               tg_https_status_name(https_status));
        if (https_status == TG_HTTPS_TLS_ERROR) {
            printf(" / %s", tg_tls_status_name(tls_status));
            if (tls_status == TG_TLS_NET_ERROR) {
                printf(" / %s", tg_net_status_name(net_status));
            }
        }
        if (net_error[0] != '\0') {
            printf(" (%s)", net_error);
        }
        printf("\n");
        return 2;
    }

    printf("telegram preflight https: received %lu bytes\n",
           response_length);
    parse_status = tg_http_parse_response(response, response_length,
                                          &parsed_response);
    if (parse_status != TG_HTTP_PARSE_OK) {
        printf("telegram preflight http parse: failed: %s\n",
               tg_http_parse_status_name(parse_status));
        return 2;
    }

    printf("telegram preflight http status: %d", parsed_response.status_code);
    if (parsed_response.reason_length > 0) {
        printf(" %.*s", (int)parsed_response.reason_length,
               parsed_response.reason);
    }
    printf("\n");

    if (parsed_response.status_code < 200 ||
        parsed_response.status_code > 399) {
        puts("telegram preflight: failed");
        return 2;
    }

    puts("telegram preflight: ok");
    return 0;
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_get_me_self_test(void)
{
    static const char get_me_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":{\"id\":123456,\"is_bot\":true,\"first_name\":\"Telegram Amiga\"}}";
    tg_bot_status bot_status;
    tg_bot_call_result result;

    bot_status = tg_bot_parse_get_me_http_response(get_me_response,
                                                   (unsigned long)strlen(get_me_response),
                                                   &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram getMe self-test: failed: %s", tg_bot_status_name(bot_status));
        if (bot_status == TG_BOT_TELEGRAM_ERROR) {
            printf(" / %s", tg_telegram_status_name(result.telegram_status));
        }
        printf("\n");
        return 2;
    }

    puts("telegram getMe self-test: ok response");
    printf("telegram http status: %d\n", result.response.http_status_code);
    tg_print_telegram_response(&result.response.api);
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static void tg_print_bot_error(const char *label, tg_bot_status bot_status,
                               const tg_bot_call_result *result,
                               const char *error_buffer)
{
    printf("%s: failed: %s", label, tg_bot_status_name(bot_status));
    if (bot_status == TG_BOT_TOKEN_ERROR || bot_status == TG_BOT_PATH_ERROR ||
        bot_status == TG_BOT_TELEGRAM_ERROR) {
        printf(" / %s", tg_telegram_status_name(result->telegram_status));
    }
    if (bot_status == TG_BOT_TOKEN_ERROR &&
        result->telegram_status == TG_TELEGRAM_FILE_ERROR) {
        printf(" / %s", tg_file_status_name(result->file_status));
    }
    if (bot_status == TG_BOT_HTTPS_ERROR) {
        printf(" / %s", tg_https_status_name(result->https_status));
        if (result->https_status == TG_HTTPS_TLS_ERROR) {
            printf(" / %s", tg_tls_status_name(result->tls_status));
            if (result->tls_status == TG_TLS_NET_ERROR) {
                printf(" / %s", tg_net_status_name(result->net_status));
            }
        }
    }
    if (error_buffer != 0 && error_buffer[0] != '\0') {
        printf(" (%s)", error_buffer);
    }
    printf("\n");
}

static int tg_run_telegram_get_me_path(const char *token_file_path)
{
    tg_bot_status bot_status;
    tg_bot_call_result result;
    char error_buffer[256];
    char http_buffer[8192];
    unsigned long http_response_length;

    bot_status = tg_bot_get_me_from_token_file(token_file_path,
                                               http_buffer, sizeof(http_buffer),
                                               &http_response_length, &result,
                                               error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram getMe", bot_status, &result, error_buffer);
        return 2;
    }

    printf("telegram getMe: received %lu bytes\n", http_response_length);
    printf("telegram http status: %d\n", result.response.http_status_code);
    tg_print_telegram_response(&result.response.api);

    if (result.response.http_status_code < 200 ||
        result.response.http_status_code > 299 ||
        !result.response.api.ok) {
        return 2;
    }

    return 0;
}

static int tg_run_telegram_get_me(const tg_config *config)
{
    return tg_run_telegram_get_me_path(config->telegram_get_me_token_file_path);
}

static int tg_run_telegram_get_me_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_get_me_path(resolved_path);
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_get_updates_self_test(void)
{
    static const char updates_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":["
        "{\"update_id\":1000,\"message\":{\"message_id\":1,\"chat\":{\"id\":123,\"type\":\"private\"},\"text\":\"hello\"}},"
        "{\"update_id\":1001,\"message\":{\"message_id\":2,\"chat\":{\"id\":456,\"type\":\"private\"},\"text\":\"second\"}}"
        "]}";
    static const char empty_updates_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":[]}";
    tg_bot_status bot_status;
    tg_bot_call_result result;
    tg_bot_update_summary update;

    bot_status = tg_bot_parse_get_updates_http_response(updates_response,
                                                        (unsigned long)strlen(updates_response),
                                                        &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram getUpdates self-test: failed: %s",
               tg_bot_status_name(bot_status));
        if (bot_status == TG_BOT_TELEGRAM_ERROR) {
            printf(" / %s", tg_telegram_status_name(result.telegram_status));
        }
        printf("\n");
        return 2;
    }

    puts("telegram getUpdates self-test: ok response");
    printf("telegram http status: %d\n", result.response.http_status_code);
    tg_print_telegram_response(&result.response.api);

    bot_status = tg_bot_get_updates_first(&result, &update);
    if (bot_status != TG_BOT_OK) {
        printf("telegram getUpdates self-test: update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    tg_print_update_summary(&update);

    bot_status = tg_bot_get_updates_at(&result, 1, &update);
    if (bot_status != TG_BOT_OK || !update.has_update ||
        strcmp(update.update_id, "1001") != 0 ||
        strcmp(update.chat_id, "456") != 0) {
        printf("telegram getUpdates self-test: second update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    tg_print_update_summary(&update);

    bot_status = tg_bot_get_updates_at(&result, 2, &update);
    if (bot_status != TG_BOT_OK || update.has_update) {
        printf("telegram getUpdates self-test: end update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    bot_status = tg_bot_parse_get_updates_http_response(
        empty_updates_response,
        (unsigned long)strlen(empty_updates_response),
        &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram getUpdates empty self-test: failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    bot_status = tg_bot_get_updates_first(&result, &update);
    if (bot_status != TG_BOT_OK || update.has_update) {
        printf("telegram getUpdates empty self-test: failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    puts("telegram getUpdates empty self-test: ok");
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_print_get_updates_summaries(const tg_bot_call_result *result,
                                          unsigned long max_updates)
{
    tg_bot_status bot_status;
    tg_bot_update_summary update;
    unsigned long index;

    for (index = 0; index < max_updates; ++index) {
        bot_status = tg_bot_get_updates_at(result, index, &update);
        if (bot_status != TG_BOT_OK) {
            printf("telegram getUpdates: update failed: %s\n",
                   tg_bot_status_name(bot_status));
            return 2;
        }
        if (!update.has_update) {
            if (index == 0) {
                puts("telegram update: none");
            }
            return 0;
        }
        printf("telegram update index: %lu\n", index);
        tg_print_update_summary(&update);
    }

    printf("telegram getUpdates: summary limited to %lu updates\n",
           max_updates);
    return 0;
}

static int tg_run_telegram_get_updates_paths(const char *token_file_path,
                                             const char *offset)
{
    tg_bot_status bot_status;
    tg_bot_call_result result;
    char error_buffer[256];
    char http_buffer[16384];
    unsigned long http_response_length;

    bot_status = tg_bot_get_updates_from_token_file_with_offset(
        token_file_path,
        offset,
        http_buffer, sizeof(http_buffer), &http_response_length, &result,
        error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram getUpdates", bot_status, &result, error_buffer);
        return 2;
    }

    printf("telegram getUpdates: received %lu bytes\n", http_response_length);
    printf("telegram http status: %d\n", result.response.http_status_code);
    tg_print_telegram_response(&result.response.api);

    if (result.response.http_status_code >= 200 &&
        result.response.http_status_code <= 299 &&
        result.response.api.ok) {
        if (tg_print_get_updates_summaries(&result, 5) != 0) {
            return 2;
        }
    }

    if (result.response.http_status_code < 200 ||
        result.response.http_status_code > 299 ||
        !result.response.api.ok) {
        return 2;
    }

    return 0;
}

static int tg_run_telegram_get_updates(const tg_config *config)
{
    return tg_run_telegram_get_updates_paths(
        config->telegram_get_updates_token_file_path,
        config->telegram_get_updates_offset);
}

static int tg_run_telegram_get_updates_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_get_updates_paths(
        resolved_path, config->telegram_get_updates_default_offset);
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_read_once_state_self_test(void)
{
    static const char offset_file_path[] = "telegram-read-once-self-test.tmp";
    static const char updates_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":["
        "{\"update_id\":200,\"message\":{\"message_id\":1,\"chat\":{\"id\":123,\"type\":\"private\"},\"text\":\"read \\\"only\\\"\\nLine \\u20ac\"}},"
        "{\"update_id\":201,\"edited_message\":{\"message_id\":2,\"chat\":{\"id\":456,\"type\":\"private\"},\"text\":\"ignored\"}}"
        "]}";
    static const char expected_text[] =
        "read \"only\"\nLine " "\xe2" "\x82" "\xac";
    tg_bot_status bot_status;
    tg_json_status json_status;
    tg_bot_call_result result;
    tg_bot_update_summary update;
    char next_offset[32];
    char saved_offset[32];
    char decoded_text[128];
    unsigned long decoded_length;

    remove(offset_file_path);

    bot_status = tg_bot_parse_get_updates_http_response(
        updates_response, (unsigned long)strlen(updates_response), &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram read once state self-test: getUpdates failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    bot_status = tg_bot_get_updates_at(&result, 0, &update);
    if (bot_status != TG_BOT_OK || !update.has_update ||
        !update.has_message || !update.has_text) {
        printf("telegram read once state self-test: first update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    json_status = tg_json_string_decode(update.text, update.text_length,
                                        decoded_text, sizeof(decoded_text),
                                        &decoded_length);
    if (json_status != TG_JSON_OK || strcmp(decoded_text, expected_text) != 0) {
        printf("telegram read once state self-test: text decode failed: %s\n",
               tg_json_status_name(json_status));
        return 2;
    }
    bot_status = tg_bot_update_next_offset(&update, next_offset,
                                           sizeof(next_offset));
    if (bot_status != TG_BOT_OK || strcmp(next_offset, "201") != 0) {
        printf("telegram read once state self-test: first offset failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    if (tg_offset_state_save_file(offset_file_path, next_offset) != 0 ||
        tg_offset_state_load_file(offset_file_path, saved_offset,
                            sizeof(saved_offset)) != 0 ||
        strcmp(saved_offset, "201") != 0) {
        puts("telegram read once state self-test: first offset file failed");
        remove(offset_file_path);
        return 2;
    }

    bot_status = tg_bot_get_updates_at(&result, 1, &update);
    if (bot_status != TG_BOT_OK || !update.has_update ||
        update.has_message || update.has_text) {
        printf("telegram read once state self-test: second update failed: %s\n",
               tg_bot_status_name(bot_status));
        remove(offset_file_path);
        return 2;
    }
    bot_status = tg_bot_update_next_offset(&update, next_offset,
                                           sizeof(next_offset));
    if (bot_status != TG_BOT_OK || strcmp(next_offset, "202") != 0) {
        printf("telegram read once state self-test: second offset failed: %s\n",
               tg_bot_status_name(bot_status));
        remove(offset_file_path);
        return 2;
    }
    if (tg_offset_state_save_file(offset_file_path, next_offset) != 0 ||
        tg_offset_state_load_file(offset_file_path, saved_offset,
                            sizeof(saved_offset)) != 0 ||
        strcmp(saved_offset, "202") != 0) {
        puts("telegram read once state self-test: second offset file failed");
        remove(offset_file_path);
        return 2;
    }

    bot_status = tg_bot_get_updates_at(&result, 2, &update);
    if (bot_status != TG_BOT_OK || update.has_update) {
        printf("telegram read once state self-test: end update failed: %s\n",
               tg_bot_status_name(bot_status));
        remove(offset_file_path);
        return 2;
    }

    remove(offset_file_path);
    puts("telegram read once state self-test: ok");
    printf("telegram read once state self-test: processed 2 update(s)\n");
    printf("telegram next offset: %s\n", next_offset);
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_telegram_read_once_state_paths(const char *token_file_path,
                                                 const char *offset_file_path,
                                                 tg_read_output_mode output_mode,
                                                 const char *inbox_log_file_path,
                                                 const char *chat_state_file_path)
{
    tg_bot_status bot_status;
    tg_bot_call_result updates_result;
    tg_bot_update_summary update;
    char offset[32];
    char error_buffer[256];
    char http_buffer[16384];
    char next_offset[32];
    unsigned long http_response_length;
    unsigned long index;
    unsigned long processed_count;

    if (tg_offset_state_load_file(offset_file_path, offset, sizeof(offset)) != 0) {
        return 2;
    }
    if (output_mode == TG_READ_OUTPUT_INBOX) {
        if (offset[0] != '\0') {
            printf("inbox offset loaded: %s\n", offset);
        } else {
            puts("inbox offset loaded: none");
        }
    } else if (output_mode == TG_READ_OUTPUT_CHAT ||
               output_mode == TG_READ_OUTPUT_HUMAN) {
        /* Chat mode keeps polling quiet unless a user-visible update arrives. */
    } else {
        if (offset[0] != '\0') {
            printf("telegram offset loaded: %s\n", offset);
        } else {
            puts("telegram offset loaded: none");
        }
    }

    bot_status = tg_bot_get_updates_from_token_file_with_offset(
        token_file_path,
        offset[0] != '\0' ? offset : 0,
        http_buffer, sizeof(http_buffer), &http_response_length, &updates_result,
        error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram read once state getUpdates", bot_status,
                           &updates_result, error_buffer);
        return 2;
    }
    if (updates_result.response.http_status_code < 200 ||
        updates_result.response.http_status_code > 299 ||
        !updates_result.response.api.ok) {
        printf("telegram read once state getUpdates: http status %d\n",
               updates_result.response.http_status_code);
        tg_print_telegram_response(&updates_result.response.api);
        return 2;
    }

    processed_count = 0;
    for (index = 0; index < TG_STATE_MAX_UPDATES; ++index) {
        bot_status = tg_bot_get_updates_at(&updates_result, index, &update);
        if (bot_status != TG_BOT_OK) {
            printf("telegram read once state: update failed: %s\n",
                   tg_bot_status_name(bot_status));
            return 2;
        }
        if (!update.has_update) {
            if (index == 0) {
                if (output_mode == TG_READ_OUTPUT_INBOX) {
                    puts("inbox: no update to process");
                } else if (output_mode == TG_READ_OUTPUT_CHAT) {
                    puts("chat: no new messages");
                } else if (output_mode == TG_READ_OUTPUT_HUMAN) {
                    /* Human chat mode stays quiet when there are no updates. */
                } else {
                    puts("telegram read once state: no update to process");
                }
            } else {
                if (output_mode == TG_READ_OUTPUT_INBOX) {
                    printf("inbox processed: %lu update(s)\n", processed_count);
                } else if (output_mode == TG_READ_OUTPUT_CHAT ||
                           output_mode == TG_READ_OUTPUT_HUMAN) {
                    /* The received messages were already printed. */
                } else {
                    printf("telegram read once state: processed %lu update(s)\n",
                           processed_count);
                }
            }
            return 0;
        }

        if (output_mode == TG_READ_OUTPUT_INBOX) {
            printf("inbox item: %lu\n", index);
            tg_print_inbox_update_summary(&update);
            if (tg_append_inbox_log_line(inbox_log_file_path, &update) != 0) {
                return 2;
            }
            if (tg_update_chat_state_file(chat_state_file_path, &update) != 0) {
                return 2;
            }
        } else if (output_mode == TG_READ_OUTPUT_CHAT ||
                   output_mode == TG_READ_OUTPUT_HUMAN) {
            tg_print_chat_update_summary(&update);
            if (output_mode == TG_READ_OUTPUT_CHAT ||
                output_mode == TG_READ_OUTPUT_HUMAN) {
                if (tg_append_inbox_log_line_mode(inbox_log_file_path,
                                                  &update, 0) != 0) {
                    return 2;
                }
            }
            if (tg_update_chat_state_file_mode(chat_state_file_path,
                                               &update, 0) != 0) {
                return 2;
            }
        } else {
            printf("telegram read once state update index: %lu\n", index);
            tg_print_update_summary(&update);
        }

        bot_status = tg_bot_update_next_offset(&update, next_offset,
                                               sizeof(next_offset));
        if (bot_status != TG_BOT_OK) {
            printf("telegram read once state: next offset failed: %s\n",
                   tg_bot_status_name(bot_status));
            return 2;
        }
        if (output_mode == TG_READ_OUTPUT_INBOX) {
            printf("inbox next offset: %s\n", next_offset);
        } else if (output_mode == TG_READ_OUTPUT_CHAT ||
                   output_mode == TG_READ_OUTPUT_HUMAN) {
            /* Keep the persisted offset out of the interactive transcript. */
        } else {
            printf("telegram next offset: %s\n", next_offset);
        }
        if (tg_offset_state_save_file_mode(
                offset_file_path, next_offset,
                output_mode != TG_READ_OUTPUT_CHAT &&
                output_mode != TG_READ_OUTPUT_HUMAN) != 0) {
            return 2;
        }
        ++processed_count;
    }

    bot_status = tg_bot_get_updates_at(&updates_result, TG_STATE_MAX_UPDATES,
                                       &update);
    if (bot_status != TG_BOT_OK) {
        printf("telegram read once state: update limit check failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    if (update.has_update) {
        if (output_mode == TG_READ_OUTPUT_INBOX) {
            printf("inbox update limit reached: %lu\n", TG_STATE_MAX_UPDATES);
        } else if (output_mode == TG_READ_OUTPUT_CHAT) {
            printf("chat: update limit reached: %lu\n", TG_STATE_MAX_UPDATES);
        } else if (output_mode == TG_READ_OUTPUT_HUMAN) {
            puts("more messages pending");
        } else {
            printf("telegram read once state: update limit reached: %lu\n",
                   TG_STATE_MAX_UPDATES);
        }
    }
    if (output_mode == TG_READ_OUTPUT_INBOX) {
        printf("inbox processed: %lu update(s)\n", processed_count);
    } else if (output_mode == TG_READ_OUTPUT_CHAT ||
               output_mode == TG_READ_OUTPUT_HUMAN) {
        /* The received messages were already printed. */
    } else {
        printf("telegram read once state: processed %lu update(s)\n",
               processed_count);
    }
    return 0;
}

static int tg_run_telegram_read_once_state(const tg_config *config)
{
    return tg_run_telegram_read_once_state_paths(
        config->telegram_read_once_state_token_file_path,
        config->telegram_read_once_state_offset_file_path,
        TG_READ_OUTPUT_STATE,
        0,
        0);
}

static int tg_run_telegram_read_once_state_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_read_once_state_paths(
        resolved_path,
        config->telegram_read_once_state_default_offset_file_path,
        TG_READ_OUTPUT_STATE,
        0,
        0);
}

static int tg_run_telegram_read_loop_paths(const char *token_file_path,
                                           const char *offset_file_path,
                                           const char *poll_seconds_text,
                                           const char *max_iterations_text,
                                           tg_read_output_mode output_mode,
                                           const char *inbox_log_file_path,
                                           const char *chat_state_file_path)
{
    unsigned long poll_seconds;
    unsigned long max_iterations;
    unsigned long iteration;
    int rc;

    if (tg_parse_decimal_ulong(poll_seconds_text, &poll_seconds) != 0) {
        puts("telegram read loop: invalid poll seconds");
        return 2;
    }
    if (tg_parse_decimal_ulong(max_iterations_text, &max_iterations) != 0) {
        puts("telegram read loop: invalid max iterations");
        return 2;
    }
    if (poll_seconds > 3600UL) {
        puts("telegram read loop: poll seconds must be <= 3600");
        return 2;
    }
    if (max_iterations == 0UL || max_iterations > 10000UL) {
        puts("telegram read loop: max iterations must be between 1 and 10000");
        return 2;
    }

    for (iteration = 0; iteration < max_iterations; ++iteration) {
        printf("telegram read loop iteration: %lu/%lu\n",
               iteration + 1UL, max_iterations);
        rc = tg_run_telegram_read_once_state_paths(
            token_file_path,
            offset_file_path,
            output_mode,
            inbox_log_file_path,
            chat_state_file_path);
        if (rc != 0) {
            return rc;
        }
        if (iteration + 1UL < max_iterations && poll_seconds > 0UL) {
            printf("telegram read loop sleep: %lu seconds\n", poll_seconds);
            tg_platform_sleep_seconds(poll_seconds);
        }
    }

    return 0;
}

static int tg_run_telegram_read_loop(const tg_config *config)
{
    return tg_run_telegram_read_loop_paths(
        config->telegram_read_loop_token_file_path,
        config->telegram_read_loop_offset_file_path,
        config->telegram_read_loop_poll_seconds,
        config->telegram_read_loop_max_iterations,
        TG_READ_OUTPUT_STATE,
        0,
        0);
}

static int tg_run_telegram_read_loop_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_read_loop_paths(
        resolved_path,
        config->telegram_read_loop_default_offset_file_path,
        config->telegram_read_loop_default_poll_seconds,
        config->telegram_read_loop_default_max_iterations,
        TG_READ_OUTPUT_STATE,
        0,
        0);
}

static int tg_run_telegram_inbox(const tg_config *config)
{
    return tg_run_telegram_read_once_state_paths(
        config->telegram_inbox_token_file_path,
        config->telegram_inbox_offset_file_path,
        TG_READ_OUTPUT_INBOX,
        config->inbox_log_file_path,
        config->chat_state_file_path);
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_inbox_self_test(void)
{
    static const char log_file_path[] = "telegram-inbox-self-test.log";
    static const char chat_state_file_path[] = "telegram-chats-self-test.txt";
    static const char updates_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":["
        "{\"update_id\":300,\"message\":{\"message_id\":1,"
        "\"date\":1710000000,"
        "\"from\":{\"id\":42,\"first_name\":\"Michele\",\"username\":\"kaffeine\"},"
        "\"chat\":{\"id\":123,\"type\":\"private\"},"
        "\"text\":\"Inbox \\\"test\\\"\\nLine\"}},"
        "{\"update_id\":301,\"message\":{\"message_id\":2,"
        "\"date\":1710000001,"
        "\"from\":{\"id\":42,\"first_name\":\"Michele\",\"username\":\"kaffeine\"},"
        "\"chat\":{\"id\":123,\"type\":\"private\"},"
        "\"photo\":[{\"file_id\":\"abc\",\"width\":100,\"height\":100}]}}"
        "]}";
    static const char expected_text[] = "Inbox \"test\"\nLine";
    tg_bot_status bot_status;
    tg_json_status json_status;
    tg_bot_call_result result;
    tg_bot_update_summary update;
    tg_bot_update_summary photo_update;
    char next_offset[32];
    char decoded_text[128];
    char log_text[1024];
    char chat_state_text[1024];
    unsigned long decoded_length;
    unsigned long log_length;
    unsigned long chat_state_length;

    remove(log_file_path);
    remove(chat_state_file_path);

    bot_status = tg_bot_parse_get_updates_http_response(
        updates_response, (unsigned long)strlen(updates_response), &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram inbox self-test: getUpdates failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    bot_status = tg_bot_get_updates_at(&result, 0, &update);
    if (bot_status != TG_BOT_OK || !update.has_update ||
        !update.has_message || !update.has_sender_name ||
        strcmp(update.sender_name, "kaffeine") != 0 || !update.has_text ||
        !update.has_date || strcmp(update.date, "1710000000") != 0 ||
        update.message_kind != TG_BOT_MESSAGE_TEXT) {
        printf("telegram inbox self-test: update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    json_status = tg_json_string_decode(update.text, update.text_length,
                                        decoded_text, sizeof(decoded_text),
                                        &decoded_length);
    if (json_status != TG_JSON_OK || strcmp(decoded_text, expected_text) != 0) {
        printf("telegram inbox self-test: text decode failed: %s\n",
               tg_json_status_name(json_status));
        return 2;
    }
    bot_status = tg_bot_update_next_offset(&update, next_offset,
                                           sizeof(next_offset));
    if (bot_status != TG_BOT_OK || strcmp(next_offset, "301") != 0) {
        printf("telegram inbox self-test: offset failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    bot_status = tg_bot_get_updates_at(&result, 1, &photo_update);
    if (bot_status != TG_BOT_OK || !photo_update.has_update ||
        !photo_update.has_message || photo_update.has_text ||
        photo_update.message_kind != TG_BOT_MESSAGE_PHOTO ||
        !photo_update.has_date || strcmp(photo_update.date, "1710000001") != 0) {
        printf("telegram inbox self-test: photo update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    tg_print_inbox_update_summary(&update);
    printf("inbox next offset: %s\n", next_offset);
    tg_print_inbox_update_summary(&photo_update);
    if (tg_append_inbox_log_line(log_file_path, &update) != 0 ||
        tg_append_inbox_log_line(log_file_path, &photo_update) != 0 ||
        tg_update_chat_state_file(chat_state_file_path, &update) != 0 ||
        tg_update_chat_state_file(chat_state_file_path, &photo_update) != 0 ||
        tg_file_read_text(log_file_path, log_text, sizeof(log_text),
                          &log_length) != TG_FILE_OK ||
        strstr(log_text, " | text | Inbox \"test\" Line") == 0 ||
        strstr(log_text, " | photo | <photo>") == 0 ||
        tg_file_read_text(chat_state_file_path, chat_state_text,
                          sizeof(chat_state_text),
                          &chat_state_length) != TG_FILE_OK ||
        strstr(chat_state_text, "123|kaffeine|2024-03-09 16:00:01 UTC|<photo>\n") == 0) {
        puts("telegram inbox self-test: local state failed");
        remove(log_file_path);
        remove(chat_state_file_path);
        return 2;
    }
    remove(log_file_path);
    remove(chat_state_file_path);
    puts("telegram inbox self-test: ok");
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_telegram_inbox_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_read_once_state_paths(
        resolved_path,
        config->telegram_inbox_default_offset_file_path,
        TG_READ_OUTPUT_INBOX,
        config->inbox_log_file_path,
        config->chat_state_file_path);
}

static int tg_run_telegram_inbox_loop(const tg_config *config)
{
    return tg_run_telegram_read_loop_paths(
        config->telegram_inbox_loop_token_file_path,
        config->telegram_inbox_loop_offset_file_path,
        config->telegram_inbox_loop_poll_seconds,
        config->telegram_inbox_loop_max_iterations,
        TG_READ_OUTPUT_INBOX,
        config->inbox_log_file_path,
        config->chat_state_file_path);
}

static int tg_run_telegram_inbox_loop_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_read_loop_paths(
        resolved_path,
        config->telegram_inbox_loop_default_offset_file_path,
        config->telegram_inbox_loop_default_poll_seconds,
        config->telegram_inbox_loop_default_max_iterations,
        TG_READ_OUTPUT_INBOX,
        config->inbox_log_file_path,
        config->chat_state_file_path);
}

static int tg_run_telegram_session_paths(const char *token_file_path,
                                         const char *offset_file_path,
                                         const char *inbox_log_file_path,
                                         const char *chat_state_file_path)
{
    puts("telegram manual session: receive only");
    printf("telegram inbox log file: %s\n", inbox_log_file_path);
    printf("telegram chat state file: %s\n", chat_state_file_path);
    return tg_run_telegram_read_once_state_paths(
        token_file_path,
        offset_file_path,
        TG_READ_OUTPUT_INBOX,
        inbox_log_file_path,
        chat_state_file_path);
}

static int tg_run_telegram_session(const tg_config *config)
{
    return tg_run_telegram_session_paths(
        config->telegram_session_token_file_path,
        config->telegram_session_offset_file_path,
        config->telegram_session_inbox_log_file_path,
        config->telegram_session_chat_state_file_path);
}

static int tg_run_telegram_session_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_session_paths(
        resolved_path,
        config->telegram_session_default_offset_file_path,
        config->telegram_session_default_inbox_log_file_path,
        config->telegram_session_default_chat_state_file_path);
}

static int tg_run_telegram_session_loop_paths(const char *token_file_path,
                                              const char *offset_file_path,
                                              const char *inbox_log_file_path,
                                              const char *chat_state_file_path,
                                              const char *poll_seconds_text,
                                              const char *max_iterations_text)
{
    puts("telegram manual session loop: receive only");
    printf("telegram inbox log file: %s\n", inbox_log_file_path);
    printf("telegram chat state file: %s\n", chat_state_file_path);
    return tg_run_telegram_read_loop_paths(
        token_file_path,
        offset_file_path,
        poll_seconds_text,
        max_iterations_text,
        TG_READ_OUTPUT_INBOX,
        inbox_log_file_path,
        chat_state_file_path);
}

static int tg_run_telegram_session_loop(const tg_config *config)
{
    return tg_run_telegram_session_loop_paths(
        config->telegram_session_loop_token_file_path,
        config->telegram_session_loop_offset_file_path,
        config->telegram_session_loop_inbox_log_file_path,
        config->telegram_session_loop_chat_state_file_path,
        config->telegram_session_loop_poll_seconds,
        config->telegram_session_loop_max_iterations);
}

static int tg_run_telegram_session_loop_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_session_loop_paths(
        resolved_path,
        config->telegram_session_loop_default_offset_file_path,
        config->telegram_session_loop_default_inbox_log_file_path,
        config->telegram_session_loop_default_chat_state_file_path,
        config->telegram_session_loop_default_poll_seconds,
        config->telegram_session_loop_default_max_iterations);
}

static int tg_run_telegram_chats(const tg_config *config)
{
    return tg_client_state_for_each_line(config->telegram_chats_file_path,
                                         tg_print_chat_line_callback, 0, 0);
}

static int tg_run_telegram_chats_default(const tg_config *config)
{
    char chats_path[256];
    const char *resolved_path;

    resolved_path = tg_default_named_file_path(
        config, tg_default_chat_state_file_name, "chat state",
        chats_path, sizeof(chats_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram chat state file: %s\n", resolved_path);
    return tg_client_state_for_each_line(resolved_path,
                                         tg_print_chat_line_callback, 0, 1);
}

static int tg_run_telegram_manual_client_paths(const char *token_file_path,
                                               const char *offset_file_path,
                                               const char *inbox_log_file_path,
                                               const char *chat_state_file_path,
                                               const char *poll_seconds_text,
                                               const char *max_iterations_text)
{
    int rc;

    puts("telegram manual client: receive-only preview");
    rc = tg_run_telegram_session_loop_paths(token_file_path,
                                            offset_file_path,
                                            inbox_log_file_path,
                                            chat_state_file_path,
                                            poll_seconds_text,
                                            max_iterations_text);
    if (rc != 0) {
        return rc;
    }

    puts("telegram manual client chats:");
    rc = tg_client_state_for_each_line(chat_state_file_path,
                                       tg_print_chat_line_callback, 0, 1);
    if (rc != 0) {
        return rc;
    }

    puts("telegram manual client reply commands:");
    puts("  TelegramAmiga --telegram-reply-default 1 \"Hello from Amiga\"");
    puts("  TelegramAmiga --telegram-send-last-default \"Hello from Amiga\"");
    return 0;
}

static int tg_run_telegram_manual_client(const tg_config *config)
{
    return tg_run_telegram_manual_client_paths(
        config->telegram_manual_client_token_file_path,
        config->telegram_manual_client_offset_file_path,
        config->telegram_manual_client_inbox_log_file_path,
        config->telegram_manual_client_chat_state_file_path,
        config->telegram_manual_client_poll_seconds,
        config->telegram_manual_client_max_iterations);
}

static int tg_run_telegram_manual_client_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_manual_client_paths(
        resolved_path,
        config->telegram_manual_client_default_offset_file_path,
        config->telegram_manual_client_default_inbox_log_file_path,
        config->telegram_manual_client_default_chat_state_file_path,
        config->telegram_manual_client_default_poll_seconds,
        config->telegram_manual_client_default_max_iterations);
}

static int tg_resolve_client_default_paths(const tg_config *config,
                                           const char **offset_file_path,
                                           const char **inbox_log_file_path,
                                           const char **chat_state_file_path,
                                           const char **selected_chat_file_path,
                                           char *offset_buffer,
                                           unsigned long offset_buffer_size,
                                           char *inbox_buffer,
                                           unsigned long inbox_buffer_size,
                                           char *chat_buffer,
                                           unsigned long chat_buffer_size,
                                           char *selected_chat_buffer,
                                           unsigned long selected_chat_buffer_size)
{
    *offset_file_path = tg_default_named_file_path(
        config, tg_default_offset_file_name, "offset",
        offset_buffer, offset_buffer_size);
    *inbox_log_file_path = tg_default_named_file_path(
        config, tg_default_inbox_log_file_name, "inbox log",
        inbox_buffer, inbox_buffer_size);
    *chat_state_file_path = tg_default_named_file_path(
        config, tg_default_chat_state_file_name, "chat state",
        chat_buffer, chat_buffer_size);
    *selected_chat_file_path = tg_default_named_file_path(
        config, tg_default_selected_chat_file_name, "selected chat",
        selected_chat_buffer, selected_chat_buffer_size);
    if (*offset_file_path == 0 || *inbox_log_file_path == 0 ||
        *chat_state_file_path == 0 || *selected_chat_file_path == 0) {
        return 2;
    }
    return 0;
}

static int tg_run_telegram_client_paths(const tg_config *config,
                                        const char *token_file_path,
                                        const char *poll_seconds_text,
                                        const char *max_iterations_text)
{
    char offset_path[256];
    char inbox_path[256];
    char chats_path[256];
    char selected_chat_path[256];
    const char *resolved_offset_path;
    const char *resolved_inbox_path;
    const char *resolved_chats_path;
    const char *resolved_selected_chat_path;

    if (poll_seconds_text == 0) {
        poll_seconds_text = tg_default_poll_seconds_text;
    }
    if (max_iterations_text == 0) {
        max_iterations_text = tg_default_max_iterations_text;
    }
    if (tg_resolve_client_default_paths(config,
                                        &resolved_offset_path,
                                        &resolved_inbox_path,
                                        &resolved_chats_path,
                                        &resolved_selected_chat_path,
                                        offset_path, sizeof(offset_path),
                                        inbox_path, sizeof(inbox_path),
                                        chats_path, sizeof(chats_path),
                                        selected_chat_path,
                                        sizeof(selected_chat_path)) != 0) {
        return 2;
    }
    (void)resolved_selected_chat_path;

    printf("telegram offset file: %s\n", resolved_offset_path);
    printf("telegram inbox log file: %s\n", resolved_inbox_path);
    printf("telegram chat state file: %s\n", resolved_chats_path);
    return tg_run_telegram_manual_client_paths(
        token_file_path,
        resolved_offset_path,
        resolved_inbox_path,
        resolved_chats_path,
        poll_seconds_text,
        max_iterations_text);
}

static int tg_run_telegram_client(const tg_config *config)
{
    return tg_run_telegram_client_paths(
        config,
        config->telegram_client_token_file_path,
        config->telegram_client_poll_seconds,
        config->telegram_client_max_iterations);
}

static int tg_run_telegram_client_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_client_paths(
        config,
        resolved_path,
        config->telegram_client_default_poll_seconds,
        config->telegram_client_default_max_iterations);
}

static int tg_run_telegram_send_chat_path(const char *token_file_path,
                                          const char *chats_file_path,
                                          const char *index_text,
                                          const char *text);

static int tg_run_telegram_send_message_path(const char *token_file_path,
                                             const char *chat_id,
                                             const char *text);
static int tg_run_telegram_send_message_path_verbose(const char *token_file_path,
                                                     const char *chat_id,
                                                     const char *text,
                                                     int verbose);

static int tg_text_client_poll_once_callback(
    const tg_text_client_config *client_config,
    void *callback_context)
{
    (void)callback_context;
    return tg_run_telegram_read_once_state_paths(
        client_config->token_file_path,
        client_config->offset_file_path,
        TG_READ_OUTPUT_CHAT,
        client_config->inbox_log_file_path,
        client_config->chat_state_file_path);
}

static int tg_text_client_human_poll_once_callback(
    const tg_text_client_config *client_config,
    void *callback_context)
{
    (void)callback_context;
    return tg_run_telegram_read_once_state_paths(
        client_config->token_file_path,
        client_config->offset_file_path,
        TG_READ_OUTPUT_HUMAN,
        client_config->inbox_log_file_path,
        client_config->chat_state_file_path);
}

static int tg_app_text_client_send_chat_callback(
    const tg_text_client_config *client_config,
    const char *chat_id,
    const char *text,
    int verbose,
    void *callback_context)
{
    (void)callback_context;
    return tg_run_telegram_send_message_path_verbose(
        client_config->token_file_path, chat_id, text, verbose);
}

static int tg_app_text_client_send_index_callback(
    const tg_text_client_config *client_config,
    const char *index_text,
    const char *text,
    void *callback_context)
{
    unsigned long index;
    char chat_id[64];

    (void)callback_context;
    if (tg_parse_decimal_ulong(index_text, &index) != 0 || index == 0UL) {
        puts("telegram text client: invalid chat index");
        return 2;
    }
    if (tg_client_state_find_chat_id_by_index(client_config->chat_state_file_path,
                                              index, chat_id,
                                              sizeof(chat_id)) != 0) {
        return 2;
    }
    return tg_run_telegram_send_message_path_verbose(
        client_config->token_file_path, chat_id, text, 0);
}

static int tg_run_telegram_client_console_paths(const tg_config *config,
                                                const char *token_file_path,
                                                const char *poll_seconds_text,
                                                const char *max_iterations_text)
{
    char offset_path[256];
    char inbox_path[256];
    char chats_path[256];
    char selected_chat_path[256];
    const char *resolved_offset_path;
    const char *resolved_inbox_path;
    const char *resolved_chats_path;
    const char *resolved_selected_chat_path;
    tg_text_client_config text_client_config;

    if (poll_seconds_text == 0) {
        poll_seconds_text = tg_console_poll_seconds_text;
    }
    if (max_iterations_text == 0) {
        max_iterations_text = tg_console_max_iterations_text;
    }
    if (tg_resolve_client_default_paths(config,
                                        &resolved_offset_path,
                                        &resolved_inbox_path,
                                        &resolved_chats_path,
                                        &resolved_selected_chat_path,
                                        offset_path, sizeof(offset_path),
                                        inbox_path, sizeof(inbox_path),
                                        chats_path, sizeof(chats_path),
                                        selected_chat_path,
                                        sizeof(selected_chat_path)) != 0) {
        return 2;
    }

    text_client_config.token_file_path = token_file_path;
    text_client_config.offset_file_path = resolved_offset_path;
    text_client_config.inbox_log_file_path = resolved_inbox_path;
    text_client_config.chat_state_file_path = resolved_chats_path;
    text_client_config.selected_chat_file_path = resolved_selected_chat_path;
    text_client_config.poll_seconds_text = poll_seconds_text;
    text_client_config.max_iterations_text = max_iterations_text;
    text_client_config.poll_once = tg_text_client_poll_once_callback;
    text_client_config.send_chat_id = tg_app_text_client_send_chat_callback;
    text_client_config.send_chat_index = tg_app_text_client_send_index_callback;
    text_client_config.callback_context = 0;

    return tg_text_client_run(&text_client_config);
}

static int tg_run_telegram_client_console(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_client_console_paths(
        config,
        resolved_path,
        config->telegram_client_console_poll_seconds,
        config->telegram_client_console_max_iterations);
}

static int tg_run_telegram_human_chat(const tg_config *config)
{
    char token_path[256];
    char offset_path[256];
    char inbox_path[256];
    char chats_path[256];
    char selected_chat_path[256];
    const char *resolved_token_path;
    const char *resolved_offset_path;
    const char *resolved_inbox_path;
    const char *resolved_chats_path;
    const char *resolved_selected_chat_path;
    tg_text_client_config text_client_config;

    resolved_token_path = tg_default_token_file_path(config, token_path,
                                                     sizeof(token_path));
    if (resolved_token_path == 0) {
        return 2;
    }
    if (tg_resolve_client_default_paths(config,
                                        &resolved_offset_path,
                                        &resolved_inbox_path,
                                        &resolved_chats_path,
                                        &resolved_selected_chat_path,
                                        offset_path, sizeof(offset_path),
                                        inbox_path, sizeof(inbox_path),
                                        chats_path, sizeof(chats_path),
                                        selected_chat_path,
                                        sizeof(selected_chat_path)) != 0) {
        return 2;
    }

    text_client_config.token_file_path = resolved_token_path;
    text_client_config.offset_file_path = resolved_offset_path;
    text_client_config.inbox_log_file_path = resolved_inbox_path;
    text_client_config.chat_state_file_path = resolved_chats_path;
    text_client_config.selected_chat_file_path = resolved_selected_chat_path;
    text_client_config.poll_seconds_text = config->telegram_human_chat_poll_seconds;
    text_client_config.max_iterations_text = 0;
    text_client_config.poll_once = tg_text_client_human_poll_once_callback;
    text_client_config.send_chat_id = tg_app_text_client_send_chat_callback;
    text_client_config.send_chat_index = tg_app_text_client_send_index_callback;
    text_client_config.callback_context = 0;

    return tg_text_client_run_human(&text_client_config);
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_console_self_test(void)
{
    char line[64];
    char index_text[16];
    char *message_text;
    unsigned long watch_seconds;

    strcpy(line, " \tread \r\n");
    tg_console_trim_ascii_space(line);
    if (strcmp(line, "read") != 0) {
        puts("telegram console self-test: trim failed");
        return 2;
    }

    strcpy(line, "reply 42 Hello console");
    message_text = 0;
    if (tg_console_parse_reply_command(line, 5, index_text,
                                       sizeof(index_text),
                                       &message_text) != 0 ||
        strcmp(index_text, "42") != 0 ||
        message_text == 0 ||
        strcmp(message_text, "Hello console") != 0) {
        puts("telegram console self-test: reply parse failed");
        return 2;
    }
    strcpy(line, "send 42");
    if (tg_console_parse_reply_command(line, 4, index_text,
                                       sizeof(index_text),
                                       &message_text) == 0) {
        puts("telegram console self-test: empty send text accepted");
        return 2;
    }

    strcpy(line, "chat 12");
    if (tg_console_parse_index_command(line, 4, index_text,
                                       sizeof(index_text)) != 0 ||
        strcmp(index_text, "12") != 0) {
        puts("telegram console self-test: chat parse failed");
        return 2;
    }
    strcpy(line, "open 3");
    if (tg_console_parse_index_command(line, 4, index_text,
                                       sizeof(index_text)) != 0 ||
        strcmp(index_text, "3") != 0) {
        puts("telegram console self-test: open parse failed");
        return 2;
    }
    strcpy(line, "7");
    if (tg_console_parse_index_command(line, 0, index_text,
                                       sizeof(index_text)) != 0 ||
        strcmp(index_text, "7") != 0) {
        puts("telegram console self-test: bare index parse failed");
        return 2;
    }
    strcpy(line, "7 extra");
    if (tg_console_parse_index_command(line, 0, index_text,
                                       sizeof(index_text)) == 0) {
        puts("telegram console self-test: invalid bare index accepted");
        return 2;
    }

    watch_seconds = 0UL;
    if (tg_console_parse_watch_command("/watch 7", &watch_seconds) != 0 ||
        watch_seconds != 7UL) {
        puts("telegram console self-test: watch parse failed");
        return 2;
    }
    if (tg_console_parse_watch_command("watch 9", &watch_seconds) != 0 ||
        watch_seconds != 9UL) {
        puts("telegram console self-test: console watch parse failed");
        return 2;
    }
    if (tg_console_parse_watch_command("/watch off", &watch_seconds) != 0 ||
        watch_seconds != 0UL) {
        puts("telegram console self-test: watch off parse failed");
        return 2;
    }
    if (tg_console_parse_watch_command("/watch 0", &watch_seconds) != 0 ||
        watch_seconds != 0UL) {
        puts("telegram console self-test: watch zero parse failed");
        return 2;
    }
    if (tg_console_parse_watch_command("/watch 3601", &watch_seconds) == 0) {
        puts("telegram console self-test: oversized watch accepted");
        return 2;
    }

    puts("telegram console self-test: ok");
    return 0;
}

static int tg_run_telegram_offset_state_self_test(void)
{
    static const char offset_file_path[] =
        "telegram-offset-state-self-test.tmp";
    char offset[32];

    remove(offset_file_path);
    if (tg_offset_state_load_file(offset_file_path, offset,
                                  sizeof(offset)) != 0 ||
        offset[0] != '\0') {
        puts("telegram offset state self-test: missing file failed");
        remove(offset_file_path);
        return 2;
    }
    if (tg_offset_state_save_file(offset_file_path, "12345") != 0 ||
        tg_offset_state_load_file(offset_file_path, offset,
                                  sizeof(offset)) != 0 ||
        strcmp(offset, "12345") != 0) {
        puts("telegram offset state self-test: save/load failed");
        remove(offset_file_path);
        return 2;
    }
    if (tg_file_write_text(offset_file_path, "12x\n", 4UL) != TG_FILE_OK ||
        tg_offset_state_load_file(offset_file_path, offset,
                                  sizeof(offset)) == 0) {
        puts("telegram offset state self-test: invalid offset accepted");
        remove(offset_file_path);
        return 2;
    }
    if (tg_offset_state_save_file(offset_file_path, "abc") == 0) {
        puts("telegram offset state self-test: invalid save accepted");
        remove(offset_file_path);
        return 2;
    }

    remove(offset_file_path);
    puts("telegram offset state self-test: ok");
    return 0;
}
#endif /* !TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
typedef struct tg_client_state_self_test_context {
    unsigned long count;
} tg_client_state_self_test_context;

static int tg_client_state_count_callback(unsigned long index,
                                          const char *line,
                                          unsigned long line_length,
                                          void *context)
{
    tg_client_state_self_test_context *test_context;
    char chat_id[64];
    char sender[128];
    char date[64];
    char text[512];

    test_context = (tg_client_state_self_test_context *)context;
    if (test_context == 0 ||
        tg_client_state_parse_line(line, line_length,
                                   chat_id, sizeof(chat_id),
                                   sender, sizeof(sender),
                                   date, sizeof(date),
                                   text, sizeof(text)) != 0) {
        return 1;
    }
    if (index == 1UL && strcmp(chat_id, "111") != 0) {
        return 1;
    }
    if (index == 2UL && strcmp(chat_id, "222") != 0) {
        return 1;
    }
    ++test_context->count;
    return 0;
}

static int tg_run_telegram_client_state_self_test(void)
{
    static const char chat_state_file_path[] =
        "telegram-client-state-self-test-chats.txt";
    char line[1024];
    char small_line[16];
    char chat_id[64];
    char sender[128];
    char date[64];
    char text[512];
    tg_client_state_self_test_context context;

    remove(chat_state_file_path);

    if (tg_client_state_build_line("111", "Alice|One\nTab\t",
                                   "2026|05|15", "Hello\nWorld|x",
                                   line, sizeof(line)) != 0 ||
        strcmp(line, "111|Alice One Tab |2026 05 15|Hello World x\n") != 0) {
        puts("telegram client state self-test: sanitized line failed");
        remove(chat_state_file_path);
        return 2;
    }
    if (tg_client_state_build_line("111", "Alice", "date", "text",
                                   small_line, sizeof(small_line)) == 0) {
        puts("telegram client state self-test: small buffer accepted");
        remove(chat_state_file_path);
        return 2;
    }
    if (tg_client_state_update_file_line(chat_state_file_path, "111",
                                         line, 0) != 0) {
        remove(chat_state_file_path);
        return 2;
    }

    if (tg_client_state_build_line("222", "Bob", "2026-05-15",
                                   "Second", line, sizeof(line)) != 0 ||
        tg_client_state_update_file_line(chat_state_file_path, "222",
                                         line, 0) != 0) {
        remove(chat_state_file_path);
        return 2;
    }
    if (tg_client_state_build_line("111", "Alice", "2026-05-15",
                                   "Latest", line, sizeof(line)) != 0 ||
        tg_client_state_update_file_line(chat_state_file_path, "111",
                                         line, 0) != 0) {
        remove(chat_state_file_path);
        return 2;
    }

    if (tg_client_state_find_chat_id_by_index(chat_state_file_path, 1UL,
                                              chat_id, sizeof(chat_id)) != 0 ||
        strcmp(chat_id, "111") != 0 ||
        tg_client_state_find_chat_id_by_index(chat_state_file_path, 2UL,
                                              chat_id, sizeof(chat_id)) != 0 ||
        strcmp(chat_id, "222") != 0) {
        puts("telegram client state self-test: chat index failed");
        remove(chat_state_file_path);
        return 2;
    }

    if (tg_client_state_parse_line("333|Sender|Date|Text", 20UL,
                                   chat_id, sizeof(chat_id),
                                   sender, sizeof(sender),
                                   date, sizeof(date),
                                   text, sizeof(text)) != 0 ||
        strcmp(chat_id, "333") != 0 ||
        strcmp(sender, "Sender") != 0 ||
        strcmp(date, "Date") != 0 ||
        strcmp(text, "Text") != 0) {
        puts("telegram client state self-test: parse failed");
        remove(chat_state_file_path);
        return 2;
    }

    context.count = 0UL;
    if (tg_client_state_for_each_line(chat_state_file_path,
                                      tg_client_state_count_callback,
                                      &context, 0) != 0 ||
        context.count != 2UL) {
        puts("telegram client state self-test: iteration failed");
        remove(chat_state_file_path);
        return 2;
    }

    remove(chat_state_file_path);
    puts("telegram client state self-test: ok");
    return 0;
}

static int tg_run_telegram_client_self_test(void)
{
    static const char chat_state_file_path[] = "telegram-client-self-test-chats.txt";
    static const char offset_file_path[] =
        "telegram-client-self-test-offset.txt";
    static const char inbox_log_file_path[] =
        "telegram-client-self-test-inbox.log";
    static const char selected_chat_file_path[] =
        "telegram-client-self-test-selected-chat.txt";
    static const char updates_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":["
        "{\"update_id\":400,\"message\":{\"message_id\":1,"
        "\"date\":1710000000,"
        "\"from\":{\"id\":42,\"username\":\"kaffeine\"},"
        "\"chat\":{\"id\":123,\"type\":\"private\"},"
        "\"text\":\"first\"}},"
        "{\"update_id\":401,\"message\":{\"message_id\":2,"
        "\"date\":1710000001,"
        "\"from\":{\"id\":43,\"username\":\"friend\"},"
        "\"chat\":{\"id\":456,\"type\":\"private\"},"
        "\"text\":\"second\"}},"
        "{\"update_id\":402,\"message\":{\"message_id\":3,"
        "\"date\":1710000002,"
        "\"from\":{\"id\":42,\"username\":\"kaffeine\"},"
        "\"chat\":{\"id\":123,\"type\":\"private\"},"
        "\"text\":\"latest\"}}"
        "]}";
    tg_bot_status bot_status;
    tg_bot_call_result result;
    tg_bot_update_summary update;
    char chat_id[64];
    char command_line[64];
    char command_index[16];
    char chat_command_index[16];
    char chat_display_line[768];
    char watch_command_line[32];
    char *command_text;
    unsigned long index;
    unsigned long watch_seconds;
    tg_text_client_config text_client_config;

    remove(chat_state_file_path);
    remove(offset_file_path);
    remove(inbox_log_file_path);
    remove(selected_chat_file_path);
    bot_status = tg_bot_parse_get_updates_http_response(
        updates_response, (unsigned long)strlen(updates_response), &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram client self-test: getUpdates failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    for (index = 0; index < 3UL; ++index) {
        bot_status = tg_bot_get_updates_at(&result, index, &update);
        if (bot_status != TG_BOT_OK || !update.has_update ||
            !update.has_message) {
            printf("telegram client self-test: update failed: %s\n",
                   tg_bot_status_name(bot_status));
            remove(chat_state_file_path);
            return 2;
        }
        if (tg_update_chat_state_file(chat_state_file_path, &update) != 0) {
            remove(chat_state_file_path);
            return 2;
        }
    }

    if (tg_client_state_find_chat_id_by_index(chat_state_file_path, 1UL,
                                              chat_id, sizeof(chat_id)) != 0 ||
        strcmp(chat_id, "123") != 0 ||
        tg_client_state_find_chat_id_by_index(chat_state_file_path, 2UL,
                                              chat_id, sizeof(chat_id)) != 0 ||
        strcmp(chat_id, "456") != 0) {
        puts("telegram client self-test: chat order failed");
        remove(chat_state_file_path);
        return 2;
    }
    bot_status = tg_bot_get_updates_at(&result, 2UL, &update);
    if (bot_status != TG_BOT_OK ||
        tg_build_chat_display_line(&update,
                                   chat_display_line,
                                   sizeof(chat_display_line)) != 0 ||
        strcmp(chat_display_line, "kaffeine: latest") != 0) {
        puts("telegram client self-test: chat display line failed");
        remove(chat_state_file_path);
        return 2;
    }
    strcpy(command_line, "r 2 Hello console");
    command_text = 0;
    if (tg_console_parse_reply_command(command_line, 1, command_index,
                                       sizeof(command_index),
                                       &command_text) != 0 ||
        strcmp(command_index, "2") != 0 ||
        command_text == 0 ||
        strcmp(command_text, "Hello console") != 0) {
        puts("telegram client self-test: console command parse failed");
        remove(chat_state_file_path);
        return 2;
    }
    strcpy(command_line, "chat 12");
    if (tg_console_parse_index_command(command_line, 4,
                                       chat_command_index,
                                       sizeof(chat_command_index)) != 0 ||
        strcmp(chat_command_index, "12") != 0) {
        puts("telegram client self-test: console chat command parse failed");
        remove(chat_state_file_path);
        return 2;
    }
    strcpy(watch_command_line, "/watch 7");
    watch_seconds = 0UL;
    if (tg_console_parse_watch_command(watch_command_line, &watch_seconds) != 0 ||
        watch_seconds != 7UL) {
        puts("telegram client self-test: console watch command parse failed");
        remove(chat_state_file_path);
        return 2;
    }
    strcpy(watch_command_line, "/watch off");
    if (tg_console_parse_watch_command(watch_command_line, &watch_seconds) != 0 ||
        watch_seconds != 0UL) {
        puts("telegram client self-test: console watch off parse failed");
        remove(chat_state_file_path);
        return 2;
    }
    puts("telegram client self-test status sample:");
    text_client_config.token_file_path = "telegram-client-self-test-token.txt";
    text_client_config.offset_file_path = offset_file_path;
    text_client_config.inbox_log_file_path = inbox_log_file_path;
    text_client_config.chat_state_file_path = chat_state_file_path;
    text_client_config.selected_chat_file_path = selected_chat_file_path;
    text_client_config.poll_seconds_text = "0";
    text_client_config.max_iterations_text = "1";
    text_client_config.poll_once = 0;
    text_client_config.send_chat_id = 0;
    text_client_config.send_chat_index = 0;
    text_client_config.callback_context = 0;
    if (tg_offset_state_save_file(offset_file_path, "402") != 0 ||
        tg_file_write_text(inbox_log_file_path,
                           "sample inbox line\n", 18UL) != TG_FILE_OK ||
        tg_file_write_text(selected_chat_file_path,
                           "1|123\n", 6UL) != TG_FILE_OK) {
        remove(chat_state_file_path);
        remove(offset_file_path);
        remove(inbox_log_file_path);
        remove(selected_chat_file_path);
        return 2;
    }
    tg_text_client_print_status(&text_client_config);
    if (tg_text_client_print_last_inbox_line(
            inbox_log_file_path) != 0) {
        remove(chat_state_file_path);
        remove(offset_file_path);
        remove(inbox_log_file_path);
        remove(selected_chat_file_path);
        return 2;
    }

    puts("telegram client self-test chats:");
    if (tg_text_client_print_chats(chat_state_file_path) != 0) {
        remove(chat_state_file_path);
        remove(offset_file_path);
        remove(inbox_log_file_path);
        remove(selected_chat_file_path);
        return 2;
    }
    remove(chat_state_file_path);
    remove(offset_file_path);
    remove(inbox_log_file_path);
    remove(selected_chat_file_path);
    puts("telegram client self-test: ok");
    return 0;
}

static int tg_run_telegram_echo_once_self_test(void)
{
    static const char updates_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":["
        "{\"update_id\":999,\"message\":{\"message_id\":1,\"chat\":{\"id\":123,\"type\":\"private\"},\"text\":\"hello \\\"Amiga\\\"\\nPath C:\\\\Temp \\u0041 \\u20ac \\ud83d\\ude80\"}},"
        "{\"update_id\":1000,\"message\":{\"message_id\":2,\"chat\":{\"id\":456,\"type\":\"private\"},\"text\":\"second\"}}"
        "]}";
    static const char expected_echo_text[] =
        "Echo: hello \"Amiga\"\nPath C:\\Temp A "
        "\xe2" "\x82" "\xac" " "
        "\xf0" "\x9f" "\x9a" "\x80";
    tg_bot_status bot_status;
    tg_bot_call_result result;
    tg_bot_update_summary update;
    tg_bot_update_summary end_update;
    char next_offset[32];
    char echo_text[256];
    char body[512];
    unsigned long body_length;

    bot_status = tg_bot_parse_get_updates_http_response(updates_response,
                                                        (unsigned long)strlen(updates_response),
                                                        &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once self-test: getUpdates failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    bot_status = tg_bot_get_updates_first(&result, &update);
    if (bot_status != TG_BOT_OK || !update.has_update ||
        !update.has_message || !update.has_text) {
        printf("telegram echo once self-test: update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    bot_status = tg_bot_update_next_offset(&update, next_offset, sizeof(next_offset));
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once self-test: offset failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    if (strcmp(next_offset, "1000") != 0) {
        printf("telegram echo once self-test: offset mismatch: %s\n", next_offset);
        return 2;
    }
    if (tg_build_echo_text(&update, echo_text, sizeof(echo_text)) != 0) {
        puts("telegram echo once self-test: echo text failed");
        return 2;
    }
    if (strcmp(echo_text, expected_echo_text) != 0) {
        puts("telegram echo once self-test: decoded echo text mismatch");
        printf("telegram echo text: %s\n", echo_text);
        return 2;
    }
    bot_status = tg_bot_build_send_message_body(update.chat_id, echo_text,
                                                body, sizeof(body), &body_length);
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once self-test: body failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    bot_status = tg_bot_get_updates_at(&result, 1, &update);
    if (bot_status != TG_BOT_OK || !update.has_update ||
        !update.has_message || !update.has_text) {
        printf("telegram echo once self-test: second update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    bot_status = tg_bot_update_next_offset(&update, next_offset, sizeof(next_offset));
    if (bot_status != TG_BOT_OK || strcmp(next_offset, "1001") != 0) {
        printf("telegram echo once self-test: second offset failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    if (tg_build_echo_text(&update, echo_text, sizeof(echo_text)) != 0 ||
        strcmp(echo_text, "Echo: second") != 0) {
        puts("telegram echo once self-test: second echo text failed");
        return 2;
    }
    bot_status = tg_bot_build_send_message_body(update.chat_id, echo_text,
                                                body, sizeof(body), &body_length);
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once self-test: second body failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    bot_status = tg_bot_get_updates_at(&result, 2, &end_update);
    if (bot_status != TG_BOT_OK || end_update.has_update) {
        printf("telegram echo once self-test: end update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }

    puts("telegram echo once self-test: ok");
    printf("telegram update id: %s\n", update.update_id);
    printf("telegram next offset: %s\n", next_offset);
    printf("telegram echo chat id: %s\n", update.chat_id);
    printf("telegram echo text: %s\n", echo_text);
    printf("telegram echo body bytes: %lu\n", body_length);
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_telegram_echo_once_paths(const char *token_file_path,
                                           const char *offset)
{
    tg_bot_status bot_status;
    tg_bot_call_result result;
    tg_bot_update_summary update;
    char error_buffer[256];
    char http_buffer[16384];
    char send_buffer[8192];
    char next_offset[32];
    char echo_text[512];
    unsigned long http_response_length;
    unsigned long send_response_length;

    bot_status = tg_bot_get_updates_from_token_file_with_offset(
        token_file_path,
        offset,
        http_buffer, sizeof(http_buffer), &http_response_length, &result,
        error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram echo once getUpdates", bot_status,
                           &result, error_buffer);
        return 2;
    }
    if (result.response.http_status_code < 200 ||
        result.response.http_status_code > 299 ||
        !result.response.api.ok) {
        printf("telegram echo once getUpdates: http status %d\n",
               result.response.http_status_code);
        tg_print_telegram_response(&result.response.api);
        return 2;
    }

    bot_status = tg_bot_get_updates_first(&result, &update);
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once: update failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    tg_print_update_summary(&update);
    if (!update.has_update) {
        puts("telegram echo once: no update to process");
        return 0;
    }

    bot_status = tg_bot_update_next_offset(&update, next_offset, sizeof(next_offset));
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once: next offset failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    printf("telegram next offset: %s\n", next_offset);

    if (!update.has_message || !update.has_text) {
        puts("telegram echo once: update has no text message");
        return 0;
    }
    if (tg_build_echo_text(&update, echo_text, sizeof(echo_text)) != 0) {
        puts("telegram echo once: echo text too long");
        return 2;
    }

    bot_status = tg_bot_send_message_from_token_file(
        token_file_path,
        update.chat_id, echo_text,
        send_buffer, sizeof(send_buffer), &send_response_length, &result,
        error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram echo once sendMessage", bot_status,
                           &result, error_buffer);
        return 2;
    }

    printf("telegram echo once sendMessage: received %lu bytes\n",
           send_response_length);
    printf("telegram http status: %d\n", result.response.http_status_code);
    tg_print_telegram_response(&result.response.api);
    if (result.response.http_status_code < 200 ||
        result.response.http_status_code > 299 ||
        !result.response.api.ok) {
        return 2;
    }

    return 0;
}

static int tg_run_telegram_echo_once(const tg_config *config)
{
    return tg_run_telegram_echo_once_paths(
        config->telegram_echo_once_token_file_path,
        config->telegram_echo_once_offset);
}

static int tg_run_telegram_echo_once_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_echo_once_paths(
        resolved_path, config->telegram_echo_once_default_offset);
}

static int tg_run_telegram_echo_once_state_paths(const char *token_file_path,
                                                 const char *offset_file_path)
{
    tg_bot_status bot_status;
    tg_bot_call_result updates_result;
    tg_bot_call_result send_result;
    tg_bot_update_summary update;
    char offset[32];
    char error_buffer[256];
    char http_buffer[16384];
    char send_buffer[8192];
    char next_offset[32];
    char echo_text[512];
    unsigned long http_response_length;
    unsigned long send_response_length;
    unsigned long index;
    unsigned long processed_count;
    unsigned long sent_count;

    if (tg_offset_state_load_file(offset_file_path, offset, sizeof(offset)) != 0) {
        return 2;
    }
    if (offset[0] != '\0') {
        printf("telegram offset loaded: %s\n", offset);
    } else {
        puts("telegram offset loaded: none");
    }

    bot_status = tg_bot_get_updates_from_token_file_with_offset(
        token_file_path,
        offset[0] != '\0' ? offset : 0,
        http_buffer, sizeof(http_buffer), &http_response_length, &updates_result,
        error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram echo once state getUpdates", bot_status,
                           &updates_result, error_buffer);
        return 2;
    }
    if (updates_result.response.http_status_code < 200 ||
        updates_result.response.http_status_code > 299 ||
        !updates_result.response.api.ok) {
        printf("telegram echo once state getUpdates: http status %d\n",
               updates_result.response.http_status_code);
        tg_print_telegram_response(&updates_result.response.api);
        return 2;
    }

    processed_count = 0;
    sent_count = 0;
    for (index = 0; index < TG_STATE_MAX_UPDATES; ++index) {
        bot_status = tg_bot_get_updates_at(&updates_result, index, &update);
        if (bot_status != TG_BOT_OK) {
            printf("telegram echo once state: update failed: %s\n",
                   tg_bot_status_name(bot_status));
            return 2;
        }
        if (!update.has_update) {
            if (index == 0) {
                puts("telegram echo once state: no update to process");
            } else {
                printf("telegram echo once state: processed %lu update(s), sent %lu message(s)\n",
                       processed_count, sent_count);
            }
            return 0;
        }

        printf("telegram echo once state update index: %lu\n", index);
        tg_print_update_summary(&update);

        bot_status = tg_bot_update_next_offset(&update, next_offset,
                                               sizeof(next_offset));
        if (bot_status != TG_BOT_OK) {
            printf("telegram echo once state: next offset failed: %s\n",
                   tg_bot_status_name(bot_status));
            return 2;
        }
        printf("telegram next offset: %s\n", next_offset);

        if (!update.has_message || !update.has_text) {
            puts("telegram echo once state: update has no text message");
            if (tg_offset_state_save_file(offset_file_path, next_offset) != 0) {
                return 2;
            }
            ++processed_count;
            continue;
        }
        if (tg_build_echo_text(&update, echo_text, sizeof(echo_text)) != 0) {
            puts("telegram echo once state: echo text too long");
            return 2;
        }

        bot_status = tg_bot_send_message_from_token_file(
            token_file_path,
            update.chat_id, echo_text,
            send_buffer, sizeof(send_buffer), &send_response_length, &send_result,
            error_buffer, sizeof(error_buffer));
        if (bot_status != TG_BOT_OK) {
            tg_print_bot_error("telegram echo once state sendMessage",
                               bot_status, &send_result, error_buffer);
            return 2;
        }

        printf("telegram echo once state sendMessage: received %lu bytes\n",
               send_response_length);
        printf("telegram http status: %d\n", send_result.response.http_status_code);
        tg_print_telegram_response(&send_result.response.api);
        if (send_result.response.http_status_code < 200 ||
            send_result.response.http_status_code > 299 ||
            !send_result.response.api.ok) {
            return 2;
        }

        if (tg_offset_state_save_file(offset_file_path, next_offset) != 0) {
            return 2;
        }
        ++processed_count;
        ++sent_count;
    }

    bot_status = tg_bot_get_updates_at(&updates_result, TG_STATE_MAX_UPDATES,
                                       &update);
    if (bot_status != TG_BOT_OK) {
        printf("telegram echo once state: update limit check failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    if (update.has_update) {
        printf("telegram echo once state: update limit reached: %lu\n",
               TG_STATE_MAX_UPDATES);
    }
    printf("telegram echo once state: processed %lu update(s), sent %lu message(s)\n",
           processed_count, sent_count);
    return 0;
}

static int tg_run_telegram_echo_once_state(const tg_config *config)
{
    return tg_run_telegram_echo_once_state_paths(
        config->telegram_echo_once_state_token_file_path,
        config->telegram_echo_once_state_offset_file_path);
}

static int tg_run_telegram_echo_once_state_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_echo_once_state_paths(
        resolved_path,
        config->telegram_echo_once_state_default_offset_file_path);
}

static int tg_run_telegram_echo_loop_paths(const char *token_file_path,
                                           const char *offset_file_path,
                                           const char *poll_seconds_text,
                                           const char *max_iterations_text)
{
    unsigned long poll_seconds;
    unsigned long max_iterations;
    unsigned long iteration;
    int rc;

    if (tg_parse_decimal_ulong(poll_seconds_text, &poll_seconds) != 0) {
        puts("telegram echo loop: invalid poll seconds");
        return 2;
    }
    if (tg_parse_decimal_ulong(max_iterations_text, &max_iterations) != 0) {
        puts("telegram echo loop: invalid max iterations");
        return 2;
    }
    if (poll_seconds > 3600UL) {
        puts("telegram echo loop: poll seconds must be <= 3600");
        return 2;
    }
    if (max_iterations == 0UL || max_iterations > 10000UL) {
        puts("telegram echo loop: max iterations must be between 1 and 10000");
        return 2;
    }

    for (iteration = 0; iteration < max_iterations; ++iteration) {
        printf("telegram echo loop iteration: %lu/%lu\n",
               iteration + 1UL, max_iterations);
        rc = tg_run_telegram_echo_once_state_paths(
            token_file_path,
            offset_file_path);
        if (rc != 0) {
            return rc;
        }
        if (iteration + 1UL < max_iterations && poll_seconds > 0UL) {
            printf("telegram echo loop sleep: %lu seconds\n", poll_seconds);
            tg_platform_sleep_seconds(poll_seconds);
        }
    }

    return 0;
}

static int tg_run_telegram_echo_loop(const tg_config *config)
{
    return tg_run_telegram_echo_loop_paths(
        config->telegram_echo_loop_token_file_path,
        config->telegram_echo_loop_offset_file_path,
        config->telegram_echo_loop_poll_seconds,
        config->telegram_echo_loop_max_iterations);
}

static int tg_run_telegram_echo_loop_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_echo_loop_paths(
        resolved_path,
        config->telegram_echo_loop_default_offset_file_path,
        config->telegram_echo_loop_default_poll_seconds,
        config->telegram_echo_loop_default_max_iterations);
}

#if !defined(TG_NO_SELFTEST)
static int tg_run_telegram_send_message_self_test(void)
{
    static const char send_response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":true,\"result\":{\"message_id\":42,\"chat\":{\"id\":123},\"text\":\"Hello Amiga\"}}";
    static const char text[] = "Hello \"Amiga\"\nBackslash \\";
    static const char expected_body[] =
        "{\"chat_id\":\"123\",\"text\":\"Hello \\\"Amiga\\\"\\nBackslash \\\\\"}";
    static const char expected_slash_body[] =
        "{\"chat_id\":\"123\",\"text\":\"\\\\\"}";
    tg_bot_status bot_status;
    tg_bot_call_result result;
    char body[256];
    unsigned long body_length;

    bot_status = tg_bot_build_send_message_body("123", text, body, sizeof(body),
                                                &body_length);
    if (bot_status != TG_BOT_OK) {
        printf("telegram sendMessage self-test: body failed: %s\n",
               tg_bot_status_name(bot_status));
        return 2;
    }
    if (strcmp(body, expected_body) != 0 ||
        body_length != (unsigned long)strlen(expected_body)) {
        puts("telegram sendMessage self-test: body mismatch");
        printf("telegram sendMessage body: %s\n", body);
        return 2;
    }
    bot_status = tg_bot_build_send_message_body("123", "\\",
                                                body, sizeof(body),
                                                &body_length);
    if (bot_status != TG_BOT_OK ||
        strcmp(body, expected_slash_body) != 0 ||
        body_length != (unsigned long)strlen(expected_slash_body)) {
        puts("telegram sendMessage self-test: trailing slash mismatch");
        printf("telegram sendMessage body: %s\n", body);
        return 2;
    }

    bot_status = tg_bot_parse_send_message_http_response(send_response,
                                                         (unsigned long)strlen(send_response),
                                                         &result);
    if (bot_status != TG_BOT_OK) {
        printf("telegram sendMessage self-test: failed: %s",
               tg_bot_status_name(bot_status));
        if (bot_status == TG_BOT_TELEGRAM_ERROR) {
            printf(" / %s", tg_telegram_status_name(result.telegram_status));
        }
        printf("\n");
        return 2;
    }

    printf("telegram sendMessage self-test: ok body, %lu bytes\n", body_length);
    printf("telegram http status: %d\n", result.response.http_status_code);
    tg_print_telegram_response(&result.response.api);
    return 0;
}
#endif /* !TG_NO_SELFTEST */

static int tg_run_telegram_send_message_path(const char *token_file_path,
                                             const char *chat_id,
                                             const char *text)
{
    return tg_run_telegram_send_message_path_verbose(token_file_path,
                                                     chat_id, text, 1);
}

static int tg_run_telegram_send_message_path_verbose(const char *token_file_path,
                                                     const char *chat_id,
                                                     const char *text,
                                                     int verbose)
{
    tg_bot_status bot_status;
    tg_bot_call_result result;
    char error_buffer[256];
    char http_buffer[8192];
    unsigned long http_response_length;

    bot_status = tg_bot_send_message_from_token_file(
        token_file_path,
        chat_id,
        text,
        http_buffer, sizeof(http_buffer), &http_response_length, &result,
        error_buffer, sizeof(error_buffer));
    if (bot_status != TG_BOT_OK) {
        tg_print_bot_error("telegram sendMessage", bot_status, &result, error_buffer);
        return 2;
    }

    if (verbose) {
        printf("telegram sendMessage: received %lu bytes\n",
               http_response_length);
        printf("telegram http status: %d\n",
               result.response.http_status_code);
        tg_print_telegram_response(&result.response.api);
    }

    if (result.response.http_status_code < 200 ||
        result.response.http_status_code > 299 ||
        !result.response.api.ok) {
        if (!verbose) {
            printf("telegram sendMessage: http status %d\n",
                   result.response.http_status_code);
            tg_print_telegram_response(&result.response.api);
        }
        return 2;
    }

    return 0;
}

static int tg_run_telegram_send_message(const tg_config *config)
{
    return tg_run_telegram_send_message_path(
        config->telegram_send_message_token_file_path,
        config->telegram_send_message_chat_id,
        config->telegram_send_message_text);
}

static int tg_run_telegram_send_message_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_send_message_path(
        resolved_path,
        config->telegram_send_message_default_chat_id,
        config->telegram_send_message_default_text);
}

static int tg_run_telegram_send_chat_path(const char *token_file_path,
                                          const char *chats_file_path,
                                          const char *index_text,
                                          const char *text)
{
    unsigned long index;
    char chat_id[64];

    if (tg_parse_decimal_ulong(index_text, &index) != 0 || index == 0UL) {
        puts("telegram send chat: invalid chat index");
        return 2;
    }
    if (tg_client_state_find_chat_id_by_index(chats_file_path, index,
                                              chat_id, sizeof(chat_id)) != 0) {
        return 2;
    }

    printf("telegram send chat index: %lu\n", index);
    printf("telegram send chat id: %s\n", chat_id);
    return tg_run_telegram_send_message_path(token_file_path, chat_id, text);
}

static int tg_run_telegram_send_chat(const tg_config *config)
{
    return tg_run_telegram_send_chat_path(
        config->telegram_send_chat_token_file_path,
        config->telegram_send_chat_file_path,
        config->telegram_send_chat_index,
        config->telegram_send_chat_text);
}

static int tg_run_telegram_send_chat_default(const tg_config *config)
{
    char token_path[256];
    const char *resolved_path;

    resolved_path = tg_default_token_file_path(config, token_path,
                                              sizeof(token_path));
    if (resolved_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_path);
    return tg_run_telegram_send_chat_path(
        resolved_path,
        config->telegram_send_chat_default_file_path,
        config->telegram_send_chat_default_index,
        config->telegram_send_chat_default_text);
}

static int tg_run_telegram_reply_default(const tg_config *config)
{
    char token_path[256];
    char chats_path[256];
    const char *resolved_token_path;
    const char *resolved_chats_path;

    resolved_token_path = tg_default_token_file_path(config, token_path,
                                                    sizeof(token_path));
    resolved_chats_path = tg_default_named_file_path(
        config, tg_default_chat_state_file_name, "chat state",
        chats_path, sizeof(chats_path));
    if (resolved_token_path == 0 || resolved_chats_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_token_path);
    printf("telegram chat state file: %s\n", resolved_chats_path);
    return tg_run_telegram_send_chat_path(
        resolved_token_path,
        resolved_chats_path,
        config->telegram_reply_default_index,
        config->telegram_reply_default_text);
}

static int tg_run_telegram_send_last_default(const tg_config *config)
{
    char token_path[256];
    char chats_path[256];
    const char *resolved_token_path;
    const char *resolved_chats_path;

    resolved_token_path = tg_default_token_file_path(config, token_path,
                                                    sizeof(token_path));
    resolved_chats_path = tg_default_named_file_path(
        config, tg_default_chat_state_file_name, "chat state",
        chats_path, sizeof(chats_path));
    if (resolved_token_path == 0 || resolved_chats_path == 0) {
        return 2;
    }

    printf("telegram token file: %s\n", resolved_token_path);
    printf("telegram chat state file: %s\n", resolved_chats_path);
    return tg_run_telegram_send_chat_path(
        resolved_token_path,
        resolved_chats_path,
        "1",
        config->telegram_send_last_default_text);
}

#if !defined(TG_NO_GUI)
/* The GUI model tg_app_run hands to the window, the demo and the two chat
   probes. One object, off the stack: it is over half a megabyte on a 64-bit
   build, and a copy of it in tg_app_run's frame (under which every self-test
   and the live window run) took more than half of the 1 MB AmigaOS stack on
   AROS ARM. The four users are exclusive, each returns before the next. */
static tg_gui_state tg_app_gui_state;

static tg_gui_state *tg_app_gui_state_zeroed(void)
{
    memset(&tg_app_gui_state, 0, sizeof(tg_app_gui_state));
    return &tg_app_gui_state;
}
#endif /* !TG_NO_GUI */

int tg_app_run(int argc, char **argv)
{
    tg_config config;
    tg_net_status net_status;
    unsigned long connect_timeout_seconds;
    char net_error[128];
    const char *program_name;

    /*
     * Amiga consoles (CON:) are not always reported as interactive by the C
     * runtime, so stdout can default to full buffering. Under MorphOS/Ambient
     * (IconX) that hides interactive prompts: the program looks "stuck" at the
     * last newline-flushed line while it is really waiting for keyboard input.
     * Force unbuffered stdout/stderr so every prompt appears immediately on
     * every platform.
     */
#ifdef TG_DIAG_TRACE
    tg_gui_log_enable();
    tg_gui_log("diag: tg_app_run entered");
#endif
    setvbuf(stdout, (char *)0, _IONBF, 0);
    /* In-place upgrade: drop the pre-0.0.6 leftovers (old binary name, IconX
       launcher scripts + icons) from the program's drawer before anything
       else, so Workbench never shows the stale duplicates. */
    tg_config_remove_superseded();
    setvbuf(stderr, (char *)0, _IONBF, 0);

    program_name = "TelegramAmiga";
    if (argc > 0 && argv[0] != 0) {
        program_name = argv[0];
    }

    tg_config_init(&config);
    if (tg_config_parse(&config, argc, argv) != 0) {
        tg_config_print_usage(stderr, program_name);
        return 1;
    }

    if (config.show_help) {
        tg_config_print_usage(stdout, program_name);
        return 0;
    }

    tg_log_set_level(config.log_level);
    if (config.connect_timeout_seconds != 0) {
        if (tg_parse_decimal_ulong(config.connect_timeout_seconds,
                                   &connect_timeout_seconds) != 0 ||
            connect_timeout_seconds > 3600UL) {
            fprintf(stderr, "connect timeout: invalid value\n");
            return 2;
        }
        tg_net_set_connect_timeout_seconds(connect_timeout_seconds);
    }
    tg_tls_set_certificate_validation(config.tls_verify,
                                      config.tls_ca_file,
                                      config.tls_ca_path);

    if (config.ui_color_mode != 0) {
        if (strcmp(config.ui_color_mode, "on") == 0) {
            tg_console_ui_set_color_mode(TG_UI_COLOR_ON);
        } else if (strcmp(config.ui_color_mode, "off") == 0) {
            tg_console_ui_set_color_mode(TG_UI_COLOR_OFF);
        } else if (strcmp(config.ui_color_mode, "auto") == 0) {
            tg_console_ui_set_color_mode(TG_UI_COLOR_AUTO);
        } else {
            fprintf(stderr, "ui-color: use on, off or auto\n");
            return 2;
        }
    }
    if (config.ui_theme != 0) {
        if (strcmp(config.ui_theme, "dark") == 0) {
            tg_console_ui_set_theme(TG_UI_THEME_DARK);
        } else if (strcmp(config.ui_theme, "plain") == 0) {
            tg_console_ui_set_theme(TG_UI_THEME_PLAIN);
        } else {
            fprintf(stderr, "ui-theme: use dark or plain\n");
            return 2;
        }
    }
    if (config.ui_tui != 0) {
        if (strcmp(config.ui_tui, "on") == 0) {
            tg_console_tui_set_enabled(1);
        } else if (strcmp(config.ui_tui, "off") == 0) {
            tg_console_tui_set_enabled(0);
        } else {
            fprintf(stderr, "ui-tui: use on or off\n");
            return 2;
        }
    }
    if (config.ui_charset != 0) {
        if (strcmp(config.ui_charset, "latin1") == 0) {
            tg_console_ui_set_charset(TG_UI_CHARSET_LATIN1);
        } else if (strcmp(config.ui_charset, "utf8") == 0) {
            tg_console_ui_set_charset(TG_UI_CHARSET_UTF8);
        } else {
            fprintf(stderr, "ui-charset: use latin1 or utf8\n");
            return 2;
        }
    }

    if (!config.run_telegram_human_chat && !config.run_mtproto_chat_file &&
        !config.run_mtproto_start_file) {
        puts("telegram-amiga " TG_VERSION " bootstrap");
        printf("platform: %s\n", tg_platform_name());
        tg_log(TG_LOG_INFO, "core initialized");
        printf("data dir: %s\n", config.data_dir);
    }

    if (config.run_net_test) {
        net_error[0] = '\0';
        net_status = tg_net_tcp_probe(config.net_test_host, config.net_test_port,
                                      net_error, sizeof(net_error));
        if (net_status != TG_NET_OK) {
            printf("net test: %s:%s failed: %s",
                   config.net_test_host, config.net_test_port,
                   tg_net_status_name(net_status));
            if (net_error[0] != '\0') {
                printf(" (%s)", net_error);
            }
            printf("\n");
            return 2;
        }
        printf("net test: %s:%s ok\n", config.net_test_host, config.net_test_port);
    }

    if (config.run_http_test) {
        return tg_run_http_test(&config);
    }

#if defined(TG_NO_SELFTEST)
    /* Stripped build: the offline self-tests are compiled out (CI runs them
       on the host binary; on a release binary they are dead weight, felt the
       most on the 68000 package). Field diagnostics stay in: --net-test,
       --http-test, --https-test, --platform-rng-test, --mtproto-2fa-bench.
       Saying so beats silently ignoring the flag. */
    if (config.run_http_post_self_test || config.run_gui_self_test ||
        config.run_chat_engine_self_test || config.run_chat_render_self_test ||
        config.run_chat_list_self_test || config.run_gui_driver_self_test ||
        config.run_console_ui_test || config.run_console_tui_test ||
        config.run_mtproto_self_test || config.run_mtproto_self_test_fast ||
        config.run_mtproto_self_test_heavy ||
        config.run_telegram_json_self_test ||
        config.run_telegram_http_self_test ||
        config.run_telegram_get_me_self_test ||
        config.run_telegram_get_updates_self_test ||
        config.run_telegram_read_once_state_self_test ||
        config.run_telegram_inbox_self_test ||
        config.run_telegram_offset_state_self_test ||
        config.run_telegram_console_self_test ||
        config.run_telegram_client_state_self_test ||
        config.run_telegram_client_self_test ||
        config.run_telegram_text_client_self_test ||
        config.run_telegram_echo_once_self_test ||
        config.run_telegram_send_message_self_test) {
        puts("Self-tests are not compiled into this build.");
        return 2;
    }
#endif /* TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
    if (config.run_http_post_self_test) {
        return tg_run_http_post_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_https_test) {
        return tg_run_https_test(&config);
    }

    if (config.run_platform_rng_test) {
        return tg_run_platform_rng_test();
    }

#if !defined(TG_NO_SELFTEST)
#if !defined(TG_NO_GUI)
    if (config.run_gui_self_test) {
        return tg_gui_self_test();
    }
#endif

    if (config.run_chat_engine_self_test) {
        return tg_chat_engine_self_test();
    }

    if (config.run_chat_render_self_test) {
        return tg_mtproto_chat_render_self_test();
    }

    if (config.run_chat_list_self_test) {
        return tg_mtproto_chat_list_self_test();
    }

#if !defined(TG_NO_GUI)
    if (config.run_gui_driver_self_test) {
        return tg_gui_driver_self_test();
    }
#endif
#endif /* !TG_NO_SELFTEST */

#if defined(TG_NO_GUI)
    /* TUI-only build (the plain-68000 package): every GUI entry point is
       compiled out so the linker drops the window, the renderer, the chat
       driver and the JPEG decoder -- about 300 KB of code and statics that
       a 68000 would never use. */
    if (config.run_gui_window || config.run_gui_live || config.run_gui_chats ||
        config.run_gui_chats_live) {
        puts("This build has no GUI: it is the text-client package.");
        return 2;
    }
#else
    if (config.run_gui_window) {
        tg_gui_state *gui_demo = tg_app_gui_state_zeroed();

        tg_gui_demo_state(gui_demo);
        return tg_gui_run_window(gui_demo);
    }

    if (config.run_gui_live) {
        tg_gui_state *gui = tg_app_gui_state_zeroed();
        int rc;

        memset(gui, 0, sizeof(*gui));
        gui->theme = TG_GUI_THEME_DARK;
        tg_gui_photo_preferences_load("data/telegram-photos.txt",
                                      &gui->inline_photos,
                                      &gui->inline_photos_explicit,
                                      &gui->photo_dither,
                                      &gui->photo_cache_limit_mb);
        gui->selected_msg = -1; /* no transcript row highlighted at start */
        if (config.run_gui_live_debug) {
            tg_gui_log_enable();
        }
        tg_gui_log("live: start");
        /* Best-effort network refresh of the chat list, then open the live
           session (it projects the sidebar and enables the notification poll).
           If the session cannot open, fall back to a read-only sidebar so the
           window still shows the cached chats. */
        tg_gui_log("live: refresh peers start");
        tg_mtproto_gui_refresh_peer_cache(config.mtproto_auth_api_file,
                                          config.mtproto_auth_file,
                                          config.gui_chats_cache_file, stdout);
        tg_gui_log("live: refresh peers done");
        tg_gui_log("live: session open start");
        rc = tg_gui_session_open(config.mtproto_auth_api_file,
                                 config.mtproto_auth_file,
                                 config.gui_chats_cache_file, gui, stdout);
        tg_gui_log(rc == 0 ? "live: session open OK" : "live: session open FAIL");
        if (rc != 0) {
            FILE *auth_probe;

            /* No saved session at all -> drive the first-login flow in the
               window. An existing-but-unusable auth (network down, expired)
               keeps the read-only cached sidebar instead. */
            auth_probe = fopen(config.mtproto_auth_file, "rb");
            if (auth_probe == 0) {
                tg_gui_log("live: no auth -> login flow");
                tg_gui_session_login_begin(config.mtproto_auth_api_file,
                                           config.mtproto_auth_file,
                                           config.gui_chats_cache_file);
                gui->mode = TG_GUI_MODE_LOGIN_PHONE;
            } else {
                tg_gui_chat_driver gui_driver;
                tg_chat_driver driver;
                tg_chat_list_row rows[TG_CHAT_LIST_MAX];
                int count;
                int missing;

                fclose(auth_probe);
                memset(&driver, 0, sizeof(driver));
                missing = 0;
                tg_gui_chat_driver_bind(&gui_driver, gui, &driver);
                count = tg_mtproto_chat_list_parse(config.gui_chats_cache_file,
                                                   0UL, rows, TG_CHAT_LIST_MAX,
                                                   &missing);
                tg_gui_saved_messages_row(rows, &count, TG_CHAT_LIST_MAX);
                if (driver.on_chat_list_changed != 0 && count > 0) {
                    driver.on_chat_list_changed(driver.ctx, rows, count);
                }
            }
        }
        if (gui->mode == TG_GUI_MODE_LOGIN_PHONE) {
            strcpy(gui->title, "Telegram Amiga");
            strcpy(gui->status, "Enter your phone number (+...)");
        } else {
            if (gui->chat_count > 0) {
                const char *name;
                unsigned long k;

                name = gui->chats[0].name;
                for (k = 0UL; k + 1UL < (unsigned long)sizeof(gui->title) &&
                              name[k] != '\0'; ++k) {
                    gui->title[k] = name[k];
                }
                gui->title[k] = '\0';
            } else {
                strcpy(gui->title, "Telegram Amiga");
            }
            strcpy(gui->status, rc == 0 ? "Live - F1-F10 chats, Q quits"
                                       : "Offline (cache) - Q quits");
        }
        /* Open the selected (first) chat up front so the transcript is
           populated on launch instead of waiting for the first key press. */
        if (rc == 0 && gui->chat_count > 0) {
            tg_gui_log("live: open first chat start");
            (void)tg_gui_session_open_chat(
                gui->chats[gui->selected_chat].index, stdout);
            tg_gui_log("live: open first chat done");
        }
        tg_gui_log("live: run_window start");
        rc = tg_gui_run_window(gui);
        tg_gui_log("live: run_window returned");
        tg_gui_session_close();
        tg_gui_log("live: session_close done");
        return rc;
    }

    if (config.run_gui_session_tick_self) {
        tg_gui_state *gui = tg_app_gui_state_zeroed();
        int i;
        int rc;

        memset(gui, 0, sizeof(*gui));
        gui->theme = TG_GUI_THEME_DARK;
        gui->inline_photos = 1;
        gui->photo_dither = TG_GUI_PHOTO_DITHER_FULL;
        gui->photo_cache_limit_mb = TG_GUI_PHOTO_CACHE_DEFAULT_MB;
        rc = tg_gui_session_open(config.mtproto_auth_api_file,
                                 config.mtproto_auth_file,
                                 config.gui_chats_cache_file, gui, stdout);
        if (rc != 0) {
            puts("gui session tick self-test: open failed (needs a live "
                 "telegram-api.txt + telegram-auth.bin)");
            return rc;
        }
        printf("gui session tick self-test: open ok, %d chats; ticking...\n",
               gui->chat_count);
        for (i = 0; i < 35; ++i) {
            (void)tg_gui_session_tick(stdout);
        }
        tg_gui_session_close();
        puts("gui session tick self-test: done");
        return 0;
    }

    if (config.run_gui_chats) {
        tg_gui_state *gui = tg_app_gui_state_zeroed();
        tg_gui_chat_driver gui_driver;
        tg_chat_driver driver;
        tg_chat_list_row rows[TG_CHAT_LIST_MAX];
        int count;
        int missing;

        memset(gui, 0, sizeof(*gui));
        memset(&driver, 0, sizeof(driver));
        gui->theme = TG_GUI_THEME_DARK;
        gui->inline_photos = 1;
        gui->photo_dither = TG_GUI_PHOTO_DITHER_FULL;
        gui->photo_cache_limit_mb = TG_GUI_PHOTO_CACHE_DEFAULT_MB;
        missing = 0;
        /* --gui-chats-live: pull a fresh chat list from the network into the
           cache first (best-effort -- on failure we still open the window over
           whatever cache exists). --gui-chats stays cache-only. */
        if (config.run_gui_chats_live) {
            tg_mtproto_gui_refresh_peer_cache(config.mtproto_auth_api_file,
                                              config.mtproto_auth_file,
                                              config.gui_chats_cache_file,
                                              stdout);
        }
        /* Project the peer cache into the sidebar through the same GUI driver
           the live client will use. */
        tg_gui_chat_driver_bind(&gui_driver, gui, &driver);
        count = tg_mtproto_chat_list_parse(config.gui_chats_cache_file, 0UL,
                                           rows, TG_CHAT_LIST_MAX, &missing);
        tg_gui_saved_messages_row(rows, &count, TG_CHAT_LIST_MAX);
        if (driver.on_chat_list_changed != 0 && count > 0) {
            driver.on_chat_list_changed(driver.ctx, rows, count);
        }
        if (gui->chat_count > 0) {
            const char *name;
            unsigned long k;

            name = gui->chats[0].name;
            for (k = 0UL; k + 1UL < (unsigned long)sizeof(gui->title) &&
                          name[k] != '\0'; ++k) {
                gui->title[k] = name[k];
            }
            gui->title[k] = '\0';
            strcpy(gui->status, "Read-only - F1-F10 chats, Q quits");
        } else {
            strcpy(gui->title, "Telegram Amiga");
            strcpy(gui->status, missing ? "Cache chat non trovata"
                                       : "Nessuna chat in cache");
        }
        return tg_gui_run_window(gui);
    }
#endif /* !TG_NO_GUI */

#if !defined(TG_NO_SELFTEST)
    if (config.run_console_ui_test) {
        return tg_mtproto_console_ui_test(stdout);
    }

    if (config.run_console_tui_test) {
        return tg_console_tui_self_test(stdout);
    }

    if (config.run_mtproto_self_test) {
        return tg_mtproto_self_test();
    }

    if (config.run_mtproto_self_test_fast) {
        return tg_mtproto_self_test_fast();
    }

    if (config.run_mtproto_self_test_heavy) {
        return tg_mtproto_self_test_heavy();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_mtproto_2fa_bench) {
        return tg_mtproto_2fa_bench(stdout);
    }

    if (config.run_mtproto_req_pq_probe) {
        return tg_mtproto_req_pq_probe(config.mtproto_probe_host,
                                       config.mtproto_probe_port, stdout);
    }

    if (config.run_mtproto_req_dh_probe) {
        return tg_mtproto_req_dh_probe(config.mtproto_probe_host,
                                       config.mtproto_probe_port,
                                       config.mtproto_probe_dc_id, stdout);
    }

    if (config.run_mtproto_auth_send_code) {
        return tg_mtproto_auth_send_code(config.mtproto_auth_host,
                                         config.mtproto_auth_port,
                                         config.mtproto_auth_dc_id,
                                         config.mtproto_auth_api_id,
                                         config.mtproto_auth_api_hash,
                                         config.mtproto_auth_phone,
                                         config.mtproto_auth_file,
                                         config.mtproto_auth_code_hash_file,
                                         stdout);
    }

    if (config.run_mtproto_auth_send_code_file) {
        return tg_mtproto_auth_send_code_file(config.mtproto_auth_host,
                                              config.mtproto_auth_port,
                                              config.mtproto_auth_dc_id,
                                              config.mtproto_auth_api_file,
                                              config.mtproto_auth_phone,
                                              config.mtproto_auth_file,
                                              config.mtproto_auth_code_hash_file,
                                              stdout);
    }

    if (config.run_mtproto_auth_sign_in) {
        return tg_mtproto_auth_sign_in(config.mtproto_auth_host,
                                       config.mtproto_auth_port,
                                       config.mtproto_auth_api_id,
                                       config.mtproto_auth_file,
                                       config.mtproto_auth_phone,
                                       config.mtproto_auth_code_hash_file,
                                       config.mtproto_auth_code,
                                       config.mtproto_auth_dc_id,
                                       stdout);
    }

    if (config.run_mtproto_auth_sign_in_file) {
        return tg_mtproto_auth_sign_in_file(config.mtproto_auth_host,
                                            config.mtproto_auth_port,
                                            config.mtproto_auth_api_file,
                                            config.mtproto_auth_file,
                                            config.mtproto_auth_phone,
                                            config.mtproto_auth_code_hash_file,
                                            config.mtproto_auth_code,
                                            config.mtproto_auth_dc_id,
                                            stdout);
    }

    if (config.run_mtproto_auth_sign_up) {
        return tg_mtproto_auth_sign_up(config.mtproto_auth_host,
                                       config.mtproto_auth_port,
                                       config.mtproto_auth_api_id,
                                       config.mtproto_auth_file,
                                       config.mtproto_auth_phone,
                                       config.mtproto_auth_code_hash_file,
                                       config.mtproto_auth_first_name,
                                       config.mtproto_auth_last_name,
                                       config.mtproto_auth_dc_id,
                                       stdout);
    }

    if (config.run_mtproto_auth_get_config) {
        return tg_mtproto_auth_get_config(config.mtproto_auth_host,
                                          config.mtproto_auth_port,
                                          config.mtproto_auth_api_id,
                                          config.mtproto_auth_file,
                                          config.mtproto_auth_dc_id,
                                          stdout);
    }

    if (config.run_mtproto_auth_get_config_file) {
        return tg_mtproto_auth_get_config_file(config.mtproto_auth_host,
                                               config.mtproto_auth_port,
                                               config.mtproto_auth_api_file,
                                               config.mtproto_auth_file,
                                               config.mtproto_auth_dc_id,
                                               stdout);
    }

    if (config.run_mtproto_auth_get_password) {
        return tg_mtproto_auth_get_password(config.mtproto_auth_host,
                                            config.mtproto_auth_port,
                                            config.mtproto_auth_api_id,
                                            config.mtproto_auth_file,
                                            config.mtproto_auth_dc_id,
                                            stdout);
    }

    if (config.run_mtproto_auth_get_password_file) {
        return tg_mtproto_auth_get_password_file(config.mtproto_auth_host,
                                                 config.mtproto_auth_port,
                                                 config.mtproto_auth_api_file,
                                                 config.mtproto_auth_file,
                                                 config.mtproto_auth_dc_id,
                                                 stdout);
    }

    if (config.run_mtproto_auth_check_password) {
        return tg_mtproto_auth_check_password(config.mtproto_auth_host,
                                              config.mtproto_auth_port,
                                              config.mtproto_auth_api_id,
                                              config.mtproto_auth_file,
                                              config.mtproto_auth_dc_id,
                                              config.mtproto_auth_password_file,
                                              stdout);
    }

    if (config.run_mtproto_auth_check_password_file) {
        return tg_mtproto_auth_check_password_file(
            config.mtproto_auth_host,
            config.mtproto_auth_port,
            config.mtproto_auth_api_file,
            config.mtproto_auth_file,
            config.mtproto_auth_dc_id,
            config.mtproto_auth_password_file,
            stdout);
    }

    if (config.run_mtproto_auth_login_wizard_file) {
        return tg_mtproto_auth_login_wizard_file(
            config.mtproto_auth_host,
            config.mtproto_auth_port,
            config.mtproto_auth_dc_id,
            config.mtproto_auth_api_file,
            config.mtproto_auth_file,
            config.mtproto_auth_code_hash_file,
            stdout);
    }

    if (config.run_mtproto_auth_status) {
        return tg_mtproto_auth_status(config.mtproto_auth_host,
                                      config.mtproto_auth_port,
                                      config.mtproto_auth_api_id,
                                      config.mtproto_auth_file,
                                      config.mtproto_auth_dc_id,
                                      stdout);
    }

    if (config.run_mtproto_auth_status_file) {
        return tg_mtproto_auth_status_file(config.mtproto_auth_host,
                                           config.mtproto_auth_port,
                                           config.mtproto_auth_api_file,
                                           config.mtproto_auth_file,
                                           config.mtproto_auth_dc_id,
                                           stdout);
    }

    if (config.run_mtproto_auth_inspect) {
        return tg_mtproto_auth_inspect(config.mtproto_auth_file, stdout);
    }

    if (config.run_mtproto_auth_check_local_files) {
        return tg_mtproto_auth_check_local_files(
            config.mtproto_auth_api_file,
            config.mtproto_auth_file,
            config.mtproto_auth_password_file,
            config.mtproto_auth_code_hash_file,
            stdout);
    }

    if (config.run_mtproto_auth_get_self) {
        return tg_mtproto_auth_get_self(config.mtproto_auth_host,
                                        config.mtproto_auth_port,
                                        config.mtproto_auth_api_id,
                                        config.mtproto_auth_file,
                                        config.mtproto_auth_dc_id,
                                        stdout);
    }

    if (config.run_mtproto_auth_get_dialogs) {
        return tg_mtproto_auth_get_dialogs(config.mtproto_auth_host,
                                           config.mtproto_auth_port,
                                           config.mtproto_auth_api_id,
                                           config.mtproto_auth_file,
                                           config.mtproto_auth_dc_id,
                                           config.mtproto_auth_limit,
                                           stdout);
    }

    if (config.run_mtproto_auth_get_dialogs_file) {
        return tg_mtproto_auth_get_dialogs_file(config.mtproto_auth_host,
                                                config.mtproto_auth_port,
                                                config.mtproto_auth_api_file,
                                                config.mtproto_auth_file,
                                                config.mtproto_auth_dc_id,
                                                config.mtproto_auth_limit,
                                                stdout);
    }

    if (config.run_mtproto_auth_list_peers_file) {
        return tg_mtproto_auth_list_peers_file(config.mtproto_auth_host,
                                               config.mtproto_auth_port,
                                               config.mtproto_auth_api_file,
                                               config.mtproto_auth_file,
                                               config.mtproto_auth_dc_id,
                                               config.mtproto_auth_limit,
                                               config.mtproto_auth_peer_cache_file,
                                               stdout);
    }

    if (config.run_mtproto_auth_resolve_username_file) {
        return tg_mtproto_auth_resolve_username_file(
            config.mtproto_auth_host,
            config.mtproto_auth_port,
            config.mtproto_auth_api_file,
            config.mtproto_auth_file,
            config.mtproto_auth_dc_id,
            config.mtproto_auth_username,
            config.mtproto_auth_peer_cache_file,
            stdout);
    }

    if (config.run_mtproto_auth_get_history_self) {
        return tg_mtproto_auth_get_history_self(config.mtproto_auth_host,
                                                config.mtproto_auth_port,
                                                config.mtproto_auth_api_id,
                                                config.mtproto_auth_file,
                                                config.mtproto_auth_dc_id,
                                                config.mtproto_auth_limit,
                                                stdout);
    }

    if (config.run_mtproto_auth_get_history_self_file) {
        return tg_mtproto_auth_get_history_self_file(
            config.mtproto_auth_host,
            config.mtproto_auth_port,
            config.mtproto_auth_api_file,
            config.mtproto_auth_file,
            config.mtproto_auth_dc_id,
            config.mtproto_auth_limit,
            stdout);
    }

    if (config.run_mtproto_auth_get_history_peer_file) {
        return tg_mtproto_auth_get_history_peer_file(
            config.mtproto_auth_host,
            config.mtproto_auth_port,
            config.mtproto_auth_api_file,
            config.mtproto_auth_file,
            config.mtproto_auth_dc_id,
            config.mtproto_auth_peer_cache_file,
            config.mtproto_auth_peer_index,
            config.mtproto_auth_limit,
            stdout);
    }

    if (config.run_mtproto_auth_send_self) {
        return tg_mtproto_auth_send_self(config.mtproto_auth_host,
                                         config.mtproto_auth_port,
                                         config.mtproto_auth_api_id,
                                         config.mtproto_auth_file,
                                         config.mtproto_auth_dc_id,
                                         config.mtproto_auth_message,
                                         stdout);
    }

    if (config.run_mtproto_auth_send_peer_file) {
        return tg_mtproto_auth_send_peer_file(config.mtproto_auth_host,
                                              config.mtproto_auth_port,
                                              config.mtproto_auth_api_file,
                                              config.mtproto_auth_file,
                                              config.mtproto_auth_dc_id,
                                              config.mtproto_auth_peer_cache_file,
                                              config.mtproto_auth_peer_index,
                                              config.mtproto_auth_message,
                                              stdout);
    }

    if (config.run_mtproto_chat_file) {
        /* No saved login yet: run the wizard against the SAME host/port
           this command was given, so redirected endpoints (e.g. a bridge
           in front of the real DC on a hosted setup) serve the login too.
           The start-file flow cannot do this: its endpoints come from the
           DC table. */
        {
            unsigned char chat_probe_key[TG_MTPROTO_AUTH_KEY_LENGTH];
            tg_mtproto_session chat_probe_session;
            tg_mtproto_session_status chat_probe_status;

            chat_probe_status = tg_mtproto_session_load_authorization(
                config.mtproto_auth_file, &chat_probe_session,
                chat_probe_key);
            memset(chat_probe_key, 0, sizeof(chat_probe_key));
            if (chat_probe_status == TG_MTPROTO_SESSION_FILE_ERROR) {
                int wizard_rc;

                puts("No saved login found. Starting login wizard.");
                wizard_rc = tg_mtproto_auth_login_wizard_file(
                    config.mtproto_auth_host, config.mtproto_auth_port,
                    config.mtproto_auth_dc_id, config.mtproto_auth_api_file,
                    config.mtproto_auth_file,
                    config.mtproto_auth_code_hash_file != 0
                        ? config.mtproto_auth_code_hash_file
                        : "data/phone-code-hash.txt",
                    stdout);
                if (wizard_rc != 0) {
                    /* A wizard aborted after sendCode leaves a transport-only
                       key behind; the next start would then skip the wizard
                       and open an unauthorized chat session. The file did
                       not exist when we started, so removing it is safe. */
                    remove(config.mtproto_auth_file);
                    return wizard_rc;
                }
            }
        }
        return tg_mtproto_auth_chat_file(config.mtproto_auth_host,
                                         config.mtproto_auth_port,
                                         config.mtproto_auth_api_file,
                                         config.mtproto_auth_file,
                                         config.mtproto_auth_dc_id,
                                         config.mtproto_chat_peer_cache_file,
                                         stdout);
    }

    if (config.run_mtproto_start_file) {
#ifdef TG_DIAG_TRACE
        tg_gui_log("diag: dispatch -> mtproto start-file (TUI)");
#endif
        return tg_run_mtproto_start_file(config.mtproto_auth_api_file,
                                         config.mtproto_auth_file,
                                         config.mtproto_auth_code_hash_file,
                                         config.mtproto_chat_peer_cache_file);
    }

    if (config.run_mtproto_auth_forget) {
        return tg_mtproto_auth_forget(config.mtproto_auth_file,
                                      config.mtproto_auth_code_hash_file,
                                      stdout);
    }

    if (config.run_telegram_tls_status) {
        return tg_run_telegram_tls_status();
    }

    if (config.run_json_test) {
        return tg_run_json_test(&config);
    }

    if (config.run_telegram_json_test) {
        return tg_run_telegram_json_test(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_json_self_test) {
        return tg_run_telegram_json_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_path_test) {
        return tg_run_telegram_path_test(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_http_self_test) {
        return tg_run_telegram_http_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_token_file_path_test) {
        return tg_run_telegram_token_file_path_test(&config);
    }

    if (config.run_telegram_default_token_file_path_test) {
        return tg_run_telegram_default_token_file_path_test(&config);
    }

    if (config.run_telegram_preflight) {
        return tg_run_telegram_preflight(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_get_me_self_test) {
        return tg_run_telegram_get_me_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_get_me) {
        return tg_run_telegram_get_me(&config);
    }

    if (config.run_telegram_get_me_default) {
        return tg_run_telegram_get_me_default(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_get_updates_self_test) {
        return tg_run_telegram_get_updates_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_get_updates) {
        return tg_run_telegram_get_updates(&config);
    }

    if (config.run_telegram_get_updates_default) {
        return tg_run_telegram_get_updates_default(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_read_once_state_self_test) {
        return tg_run_telegram_read_once_state_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_read_once_state) {
        return tg_run_telegram_read_once_state(&config);
    }

    if (config.run_telegram_read_once_state_default) {
        return tg_run_telegram_read_once_state_default(&config);
    }

    if (config.run_telegram_read_loop) {
        return tg_run_telegram_read_loop(&config);
    }

    if (config.run_telegram_read_loop_default) {
        return tg_run_telegram_read_loop_default(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_inbox_self_test) {
        return tg_run_telegram_inbox_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_inbox) {
        return tg_run_telegram_inbox(&config);
    }

    if (config.run_telegram_inbox_default) {
        return tg_run_telegram_inbox_default(&config);
    }

    if (config.run_telegram_inbox_loop) {
        return tg_run_telegram_inbox_loop(&config);
    }

    if (config.run_telegram_inbox_loop_default) {
        return tg_run_telegram_inbox_loop_default(&config);
    }

    if (config.run_telegram_session) {
        return tg_run_telegram_session(&config);
    }

    if (config.run_telegram_session_default) {
        return tg_run_telegram_session_default(&config);
    }

    if (config.run_telegram_session_loop) {
        return tg_run_telegram_session_loop(&config);
    }

    if (config.run_telegram_session_loop_default) {
        return tg_run_telegram_session_loop_default(&config);
    }

    if (config.run_telegram_manual_client) {
        return tg_run_telegram_manual_client(&config);
    }

    if (config.run_telegram_manual_client_default) {
        return tg_run_telegram_manual_client_default(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_offset_state_self_test) {
        return tg_run_telegram_offset_state_self_test();
    }
#endif /* !TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_console_self_test) {
        return tg_run_telegram_console_self_test();
    }
#endif /* !TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_client_state_self_test) {
        return tg_run_telegram_client_state_self_test();
    }
#endif /* !TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_client_self_test) {
        return tg_run_telegram_client_self_test();
    }
#endif /* !TG_NO_SELFTEST */

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_text_client_self_test) {
        return tg_text_client_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_client) {
        return tg_run_telegram_client(&config);
    }

    if (config.run_telegram_client_default) {
        return tg_run_telegram_client_default(&config);
    }

    if (config.run_telegram_client_console) {
        return tg_run_telegram_client_console(&config);
    }

    if (config.run_telegram_human_chat) {
        return tg_run_telegram_human_chat(&config);
    }

    if (config.run_telegram_chats) {
        return tg_run_telegram_chats(&config);
    }

    if (config.run_telegram_chats_default) {
        return tg_run_telegram_chats_default(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_echo_once_self_test) {
        return tg_run_telegram_echo_once_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_echo_once) {
        return tg_run_telegram_echo_once(&config);
    }

    if (config.run_telegram_echo_once_default) {
        return tg_run_telegram_echo_once_default(&config);
    }

    if (config.run_telegram_echo_once_state) {
        return tg_run_telegram_echo_once_state(&config);
    }

    if (config.run_telegram_echo_once_state_default) {
        return tg_run_telegram_echo_once_state_default(&config);
    }

    if (config.run_telegram_echo_loop) {
        return tg_run_telegram_echo_loop(&config);
    }

    if (config.run_telegram_echo_loop_default) {
        return tg_run_telegram_echo_loop_default(&config);
    }

#if !defined(TG_NO_SELFTEST)
    if (config.run_telegram_send_message_self_test) {
        return tg_run_telegram_send_message_self_test();
    }
#endif /* !TG_NO_SELFTEST */

    if (config.run_telegram_send_message) {
        return tg_run_telegram_send_message(&config);
    }

    if (config.run_telegram_send_message_default) {
        return tg_run_telegram_send_message_default(&config);
    }

    if (config.run_telegram_send_chat) {
        return tg_run_telegram_send_chat(&config);
    }

    if (config.run_telegram_send_chat_default) {
        return tg_run_telegram_send_chat_default(&config);
    }

    if (config.run_telegram_reply_default) {
        return tg_run_telegram_reply_default(&config);
    }

    if (config.run_telegram_send_last_default) {
        return tg_run_telegram_send_last_default(&config);
    }

    return 0;
}
