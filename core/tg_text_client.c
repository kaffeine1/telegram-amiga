/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "tg_client_state.h"
#include "tg_console.h"
#include "tg_file.h"
#include "tg_offset_state.h"
#include "tg_platform.h"
#include "tg_text_client.h"
#include "tg_tls.h"
#include "tg_version.h"

static const char tg_text_client_console_poll_seconds_text[] = "0";
static const char tg_text_client_console_max_iterations_text[] = "1";
static const char tg_text_client_human_poll_seconds_text[] = "2";
static const unsigned long tg_text_client_chat_default_watch_seconds = 2UL;

static int tg_text_client_load_selected_chat(const char *path,
                                             char *index_text,
                                             unsigned long index_text_size,
                                             char *chat_id,
                                             unsigned long chat_id_size);

static const char *tg_text_client_selected_label(const char *index_text)
{
    if (index_text != 0 && strcmp(index_text, "0") == 0) {
        return "id";
    }
    return index_text;
}

static int tg_text_client_is_decimal_text(const char *text)
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

static int tg_text_client_parse_decimal_ulong(const char *text,
                                              unsigned long *value)
{
    unsigned long result;
    unsigned long digit;

    if (value == 0 || !tg_text_client_is_decimal_text(text)) {
        return 1;
    }

    result = 0UL;
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

static int tg_text_client_is_chat_id_text(const char *text)
{
    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    if (*text == '-') {
        ++text;
    }
    return tg_text_client_is_decimal_text(text);
}

static int tg_text_client_print_chat_line(unsigned long index,
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
        return 1;
    }

    printf("%lu. %s", index, chat_id);
    if (sender[0] != '\0') {
        printf(" %s", sender);
    }
    if (date[0] != '\0') {
        printf(" %s", date);
    }
    if (text[0] != '\0') {
        printf(" - %s", text);
    }
    puts("");
    return 0;
}

int tg_text_client_print_chats(const char *chat_state_file_path)
{
    puts("telegram client chats:");
    return tg_client_state_for_each_line(chat_state_file_path,
                                         tg_text_client_print_chat_line,
                                         0, 1);
}

int tg_text_client_print_last_inbox_line(const char *inbox_log_file_path)
{
    tg_file_status file_status;
    char content[16384];
    unsigned long content_length;
    unsigned long line_start;
    unsigned long line_end;

    file_status = tg_file_read_text(inbox_log_file_path, content,
                                    sizeof(content), &content_length);
    if (file_status == TG_FILE_OPEN_FAILED) {
        puts("telegram inbox last: none");
        return 0;
    }
    if (file_status != TG_FILE_OK) {
        printf("telegram inbox last: read failed: %s\n",
               tg_file_status_name(file_status));
        return 2;
    }
    if (content_length == 0UL) {
        puts("telegram inbox last: none");
        return 0;
    }

    line_end = content_length;
    while (line_end > 0UL &&
           (content[line_end - 1] == '\n' ||
            content[line_end - 1] == '\r')) {
        --line_end;
    }
    if (line_end == 0UL) {
        puts("telegram inbox last: none");
        return 0;
    }
    line_start = line_end;
    while (line_start > 0UL &&
           content[line_start - 1] != '\n' &&
           content[line_start - 1] != '\r') {
        --line_start;
    }

    printf("telegram inbox last: %.*s\n",
           (int)(line_end - line_start), content + line_start);
    return 0;
}

static void tg_text_client_print_selected_status(const char *path)
{
    char index_text[32];
    char chat_id[64];

    if (tg_text_client_load_selected_chat(path, index_text, sizeof(index_text),
                                          chat_id, sizeof(chat_id)) == 0 &&
        index_text[0] != '\0') {
        printf("telegram selected chat: %s (%s)\n",
               tg_text_client_selected_label(index_text), chat_id);
    } else {
        puts("telegram selected chat: none");
    }
}

void tg_text_client_print_status(const tg_text_client_config *client_config)
{
    char offset[32];

    if (client_config == 0) {
        return;
    }
    printf("telegram offset file: %s\n", client_config->offset_file_path);
    printf("telegram inbox log file: %s\n",
           client_config->inbox_log_file_path);
    printf("telegram chat state file: %s\n",
           client_config->chat_state_file_path);
    printf("telegram selected chat file: %s\n",
           client_config->selected_chat_file_path);
    printf("tls certificate validation requested: %s\n",
           tg_tls_certificate_validation_enabled() ? "yes" : "no");
    if (tg_offset_state_load_file(client_config->offset_file_path, offset,
                                  sizeof(offset)) == 0 &&
        offset[0] != '\0') {
        printf("telegram offset: %s\n", offset);
    } else {
        puts("telegram offset: none");
    }
    tg_text_client_print_selected_status(client_config->selected_chat_file_path);
}

static void tg_text_client_print_help(void)
{
    puts("telegram text client commands:");
    puts("  /read, /refresh, p      receive new messages");
    puts("  /watch <seconds>, /watch off");
    puts("                           auto-read while waiting at the prompt");
    puts("  /chats, l               show saved chats");
    puts("  /last                   show last inbox line");
    puts("  /status                 show local files, offset and selected chat");
    puts("  /open <index>           open a send/read chat loop");
    puts("  <index>                 open saved chat by number");
    puts("  /send <text>            send to the selected chat");
    puts("  /send-id <chat-id> <text>");
    puts("                           send directly and select that chat");
    puts("  reply <index> <text>    send to saved chat index");
    puts("  /help                   show help");
    puts("  /quit                   quit");
}

static void tg_text_client_print_chat_help(void)
{
    puts("telegram chat commands:");
    puts("  <text>                  send text to the selected chat");
    puts("  /read, /refresh, /p     receive new messages");
    puts("  /watch <seconds>        set automatic receive interval");
    puts("  /watch off              disable automatic receive");
    puts("  /chats                  show saved chats");
    puts("  /last                   show last inbox line");
    puts("  /status                 show local files, offset and selected chat");
    puts("  /help                   show chat commands");
    puts("  /back                   return to console");
    puts("  /quit                   quit");
}

static unsigned long tg_text_client_initial_watch_seconds(
    const char *poll_seconds_text, unsigned long default_seconds)
{
    unsigned long seconds;

    if (poll_seconds_text != 0 &&
        tg_text_client_parse_decimal_ulong(poll_seconds_text, &seconds) == 0 &&
        seconds > 0UL) {
        if (seconds > 3600UL) {
            return 3600UL;
        }
        return seconds;
    }
    return default_seconds;
}

static int tg_text_client_set_watch_seconds(const char *line,
                                            unsigned long *watch_seconds,
                                            const char *label)
{
    unsigned long seconds;

    if (line == 0 || watch_seconds == 0) {
        return 1;
    }
    if (tg_console_parse_watch_command(line, &seconds) != 0) {
        printf("%s: use /watch <seconds <= 3600> or /watch off\n", label);
        return 0;
    }

    *watch_seconds = seconds;
    if (*watch_seconds == 0UL) {
        printf("%s: auto-read disabled\n", label);
        return 0;
    }
    printf("%s: auto-read every %lu second(s)\n", label, *watch_seconds);
    return 0;
}

static int tg_text_client_poll_once(const tg_text_client_config *client_config)
{
    if (client_config == 0 || client_config->poll_once == 0) {
        return 2;
    }
    return client_config->poll_once(client_config,
                                    client_config->callback_context);
}

static int tg_text_client_send_to_chat_id(
    const tg_text_client_config *client_config,
    const char *chat_id,
    const char *text,
    int verbose)
{
    if (client_config == 0 || client_config->send_chat_id == 0) {
        return 2;
    }
    return client_config->send_chat_id(client_config, chat_id, text, verbose,
                                       client_config->callback_context);
}

static int tg_text_client_send_to_index(
    const tg_text_client_config *client_config,
    const char *index_text,
    const char *text)
{
    if (client_config == 0 || client_config->send_chat_index == 0) {
        return 2;
    }
    return client_config->send_chat_index(client_config, index_text, text,
                                          client_config->callback_context);
}

static int tg_text_client_load_selected_chat(const char *path,
                                             char *index_text,
                                             unsigned long index_text_size,
                                             char *chat_id,
                                             unsigned long chat_id_size)
{
    tg_file_status file_status;
    char content[128];
    unsigned long content_length;
    char *separator;
    unsigned long index_length;
    unsigned long chat_id_length;

    if (index_text == 0 || index_text_size == 0 ||
        chat_id == 0 || chat_id_size == 0) {
        return 1;
    }
    index_text[0] = '\0';
    chat_id[0] = '\0';

    file_status = tg_file_read_text(path, content, sizeof(content),
                                    &content_length);
    if (file_status == TG_FILE_OPEN_FAILED) {
        return 0;
    }
    if (file_status != TG_FILE_OK) {
        printf("telegram selected chat file: read failed: %s\n",
               tg_file_status_name(file_status));
        return 1;
    }

    tg_console_trim_ascii_space(content);
    separator = strchr(content, '|');
    if (separator == 0) {
        return 1;
    }
    *separator = '\0';
    ++separator;
    if (!tg_text_client_is_decimal_text(content) || separator[0] == '\0') {
        return 1;
    }

    index_length = (unsigned long)strlen(content);
    chat_id_length = (unsigned long)strlen(separator);
    if (index_length + 1UL > index_text_size ||
        chat_id_length + 1UL > chat_id_size) {
        return 1;
    }
    strcpy(index_text, content);
    strcpy(chat_id, separator);
    return 0;
}

static int tg_text_client_save_selected_chat(const char *path,
                                             const char *index_text,
                                             const char *chat_id)
{
    char line[128];
    unsigned long index_length;
    unsigned long chat_id_length;
    unsigned long length;
    tg_file_status file_status;

    if (path == 0 || index_text == 0 || chat_id == 0 ||
        !tg_text_client_is_decimal_text(index_text) || chat_id[0] == '\0') {
        return 1;
    }
    index_length = (unsigned long)strlen(index_text);
    chat_id_length = (unsigned long)strlen(chat_id);
    length = index_length + 1UL + chat_id_length + 1UL;
    if (length + 1UL > sizeof(line)) {
        return 1;
    }
    strcpy(line, index_text);
    line[index_length] = '|';
    strcpy(line + index_length + 1UL, chat_id);
    line[length - 1UL] = '\n';
    line[length] = '\0';

    file_status = tg_file_write_text(path, line, length);
    if (file_status != TG_FILE_OK) {
        printf("telegram selected chat file: write failed: %s\n",
               tg_file_status_name(file_status));
        return 1;
    }
    return 0;
}

static int tg_text_client_select_chat(const tg_text_client_config *client_config,
                                      const char *index_text,
                                      char *selected_index,
                                      unsigned long selected_index_size,
                                      char *selected_chat_id,
                                      unsigned long selected_chat_id_size)
{
    unsigned long index;
    unsigned long index_length;

    if (tg_text_client_parse_decimal_ulong(index_text, &index) != 0 ||
        index == 0UL) {
        puts("telegram chat: invalid chat index");
        return 1;
    }
    if (tg_client_state_find_chat_id_by_index(client_config->chat_state_file_path,
                                              index, selected_chat_id,
                                              selected_chat_id_size) != 0) {
        return 1;
    }
    index_length = (unsigned long)strlen(index_text);
    if (index_length + 1UL > selected_index_size) {
        return 1;
    }
    strcpy(selected_index, index_text);
    if (tg_text_client_save_selected_chat(client_config->selected_chat_file_path,
                                          selected_index,
                                          selected_chat_id) != 0) {
        return 1;
    }

    printf("telegram chat: selected index %s, chat %s\n",
           selected_index, selected_chat_id);
    return 0;
}

static int tg_text_client_select_chat_id(
    const tg_text_client_config *client_config,
    const char *chat_id,
    char *selected_index,
    unsigned long selected_index_size,
    char *selected_chat_id,
    unsigned long selected_chat_id_size)
{
    unsigned long chat_id_length;

    if (client_config == 0 || selected_index == 0 || selected_index_size < 2 ||
        selected_chat_id == 0 || selected_chat_id_size == 0 ||
        !tg_text_client_is_chat_id_text(chat_id)) {
        puts("telegram chat: invalid chat id");
        return 1;
    }
    chat_id_length = (unsigned long)strlen(chat_id);
    if (chat_id_length + 1UL > selected_chat_id_size) {
        puts("telegram chat: chat id too long");
        return 1;
    }

    strcpy(selected_index, "0");
    strcpy(selected_chat_id, chat_id);
    if (tg_text_client_save_selected_chat(client_config->selected_chat_file_path,
                                          selected_index,
                                          selected_chat_id) != 0) {
        return 1;
    }

    printf("telegram chat: selected chat id %s\n", selected_chat_id);
    return 0;
}

static int tg_text_client_select_chat_id_quiet(
    const tg_text_client_config *client_config,
    const char *chat_id,
    char *selected_index,
    unsigned long selected_index_size,
    char *selected_chat_id,
    unsigned long selected_chat_id_size)
{
    unsigned long chat_id_length;

    if (client_config == 0 || selected_index == 0 || selected_index_size < 2 ||
        selected_chat_id == 0 || selected_chat_id_size == 0 ||
        !tg_text_client_is_chat_id_text(chat_id)) {
        return 1;
    }
    chat_id_length = (unsigned long)strlen(chat_id);
    if (chat_id_length + 1UL > selected_chat_id_size) {
        return 1;
    }

    strcpy(selected_index, "0");
    strcpy(selected_chat_id, chat_id);
    return tg_text_client_save_selected_chat(client_config->selected_chat_file_path,
                                             selected_index,
                                             selected_chat_id);
}

static int tg_text_client_load_first_chat_id(const char *path,
                                             char *chat_id,
                                             unsigned long chat_id_size)
{
    tg_file_status file_status;
    char content[16384];
    unsigned long content_length;
    unsigned long line_start;
    unsigned long line_end;
    unsigned long line_length;
    char sender[128];
    char date[64];
    char text[512];

    if (chat_id == 0 || chat_id_size == 0) {
        return 1;
    }
    chat_id[0] = '\0';
    file_status = tg_file_read_text(path, content, sizeof(content),
                                    &content_length);
    if (file_status != TG_FILE_OK) {
        return 1;
    }

    line_start = 0UL;
    while (line_start < content_length) {
        line_end = line_start;
        while (line_end < content_length &&
               content[line_end] != '\n' &&
               content[line_end] != '\r') {
            ++line_end;
        }
        line_length = line_end - line_start;
        if (line_length > 0UL &&
            tg_client_state_parse_line(content + line_start, line_length,
                                       chat_id, chat_id_size,
                                       sender, sizeof(sender),
                                       date, sizeof(date),
                                       text, sizeof(text)) == 0) {
            return 0;
        }
        line_start = line_end;
        while (line_start < content_length &&
               (content[line_start] == '\n' ||
                content[line_start] == '\r')) {
            ++line_start;
        }
    }
    return 1;
}

static int tg_text_client_select_first_chat_quiet(
    const tg_text_client_config *client_config,
    char *selected_index,
    unsigned long selected_index_size,
    char *selected_chat_id,
    unsigned long selected_chat_id_size)
{
    char chat_id[64];
    unsigned long chat_id_length;

    if (client_config == 0 || selected_index == 0 || selected_index_size < 2 ||
        selected_chat_id == 0 || selected_chat_id_size == 0) {
        return 1;
    }
    if (tg_text_client_load_first_chat_id(client_config->chat_state_file_path,
                                          chat_id, sizeof(chat_id)) != 0) {
        return 1;
    }
    chat_id_length = (unsigned long)strlen(chat_id);
    if (chat_id_length + 1UL > selected_chat_id_size) {
        return 1;
    }

    strcpy(selected_index, "1");
    strcpy(selected_chat_id, chat_id);
    return tg_text_client_save_selected_chat(client_config->selected_chat_file_path,
                                             selected_index,
                                             selected_chat_id);
}

static int tg_text_client_parse_send_id_command(
    char *line,
    unsigned long command_length,
    char *chat_id_buffer,
    unsigned long chat_id_buffer_size,
    char **text_start)
{
    char *cursor;
    char *chat_id_start;
    unsigned long chat_id_length;

    if (line == 0 || chat_id_buffer == 0 || chat_id_buffer_size == 0 ||
        text_start == 0) {
        return 1;
    }

    cursor = line + command_length;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    chat_id_start = cursor;
    if (*cursor == '-') {
        ++cursor;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
    }
    chat_id_length = (unsigned long)(cursor - chat_id_start);
    if (chat_id_length == 0 || chat_id_length + 1UL > chat_id_buffer_size ||
        (chat_id_length == 1UL && chat_id_start[0] == '-')) {
        return 1;
    }

    memcpy(chat_id_buffer, chat_id_start, chat_id_length);
    chat_id_buffer[chat_id_length] = '\0';
    if (!tg_text_client_is_chat_id_text(chat_id_buffer)) {
        return 1;
    }
    if (*cursor != ' ' && *cursor != '\t') {
        return 1;
    }

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return 1;
    }
    *text_start = cursor;
    return 0;
}

static int tg_text_client_handle_index_send(
    const tg_text_client_config *client_config,
    char *line,
    unsigned long command_length,
    const char *usage_text)
{
    char index_text[32];
    char *reply_text;
    int rc;

    if (tg_console_parse_reply_command(line, command_length, index_text,
                                       sizeof(index_text), &reply_text) != 0) {
        printf("telegram text client: use %s\n", usage_text);
        return 0;
    }
    rc = tg_text_client_send_to_index(client_config, index_text, reply_text);
    if (rc != 0) {
        return rc;
    }
    return 0;
}

static const char *tg_text_client_send_text_from_line(char *line,
                                                      unsigned long prefix)
{
    char *text;

    text = line + prefix;
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    if (*text == '\0') {
        return 0;
    }
    return text;
}

static int tg_text_client_chat_mode(const tg_text_client_config *client_config,
                                    const char *selected_index,
                                    const char *selected_chat_id)
{
    unsigned long watch_seconds;
    char line[512];
    int rc;

    watch_seconds = tg_text_client_initial_watch_seconds(
        client_config->poll_seconds_text,
        tg_text_client_chat_default_watch_seconds);
    if (watch_seconds > 0UL) {
        printf("telegram chat: auto-read every %lu second(s)\n",
               watch_seconds);
    } else {
        puts("telegram chat: auto-read disabled");
    }
    puts("telegram chat: type text to send; /read polls; /back returns");
    tg_text_client_print_chat_help();
    rc = tg_text_client_poll_once(client_config);
    if (rc != 0) {
        return rc;
    }

    for (;;) {
        printf("tg:%s:%s> ", selected_index, selected_chat_id);
        fflush(stdout);
        if (watch_seconds > 0UL &&
            !tg_platform_stdin_readable(watch_seconds)) {
            puts("");
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (fgets(line, sizeof(line), stdin) == 0) {
            puts("telegram chat: eof");
            return 0;
        }
        tg_console_trim_ascii_space(line);
        if (line[0] == '\0') {
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strcmp(line, "/back") == 0) {
            puts("telegram chat: back");
            return 0;
        }
        if (strcmp(line, "/quit") == 0 || strcmp(line, "q") == 0) {
            puts("telegram chat: quit");
            return 1;
        }
        if (strcmp(line, "/help") == 0) {
            tg_text_client_print_chat_help();
            continue;
        }
        if (strcmp(line, "/status") == 0) {
            tg_text_client_print_status(client_config);
            continue;
        }
        if (strcmp(line, "/last") == 0) {
            rc = tg_text_client_print_last_inbox_line(
                client_config->inbox_log_file_path);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strcmp(line, "/list") == 0 || strcmp(line, "/chats") == 0) {
            rc = tg_text_client_print_chats(client_config->chat_state_file_path);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strcmp(line, "/read") == 0 ||
            strcmp(line, "/refresh") == 0 ||
            strcmp(line, "/poll") == 0 ||
            strcmp(line, "/p") == 0) {
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strncmp(line, "/watch", 6) == 0 &&
            (line[6] == '\0' || line[6] == ' ' || line[6] == '\t')) {
            if (line[6] == '\0') {
                printf("telegram chat: auto-read every %lu second(s)\n",
                       watch_seconds);
            } else if (tg_text_client_set_watch_seconds(
                           line, &watch_seconds, "telegram chat") != 0) {
                return 2;
            }
            continue;
        }
        if (line[0] == '/') {
            puts("telegram chat: unknown command, use /help");
            continue;
        }

        rc = tg_text_client_send_to_chat_id(client_config, selected_chat_id,
                                            line, 0);
        if (rc != 0) {
            return rc;
        }
        rc = tg_text_client_poll_once(client_config);
        if (rc != 0) {
            return rc;
        }
    }
}

int tg_text_client_run(const tg_text_client_config *client_config)
{
    char line[512];
    char index_text[32];
    char selected_index[32];
    char selected_chat_id[64];
    const char *poll_seconds_text;
    const char *max_iterations_text;
    const char *send_text;
    char send_chat_id[64];
    char *send_id_text;
    unsigned long watch_seconds;
    int index_is_ready;
    int rc;

    if (client_config == 0 || client_config->token_file_path == 0 ||
        client_config->offset_file_path == 0 ||
        client_config->inbox_log_file_path == 0 ||
        client_config->chat_state_file_path == 0 ||
        client_config->selected_chat_file_path == 0) {
        return 2;
    }

    poll_seconds_text = client_config->poll_seconds_text;
    max_iterations_text = client_config->max_iterations_text;
    if (poll_seconds_text == 0) {
        poll_seconds_text = tg_text_client_console_poll_seconds_text;
    }
    if (max_iterations_text == 0) {
        max_iterations_text = tg_text_client_console_max_iterations_text;
    }
    (void)max_iterations_text;
    watch_seconds = tg_text_client_initial_watch_seconds(poll_seconds_text,
                                                         0UL);
    selected_index[0] = '\0';
    selected_chat_id[0] = '\0';
    if (tg_text_client_load_selected_chat(client_config->selected_chat_file_path,
                                          selected_index,
                                          sizeof(selected_index),
                                          selected_chat_id,
                                          sizeof(selected_chat_id)) != 0) {
        puts("telegram selected chat file: invalid, ignoring");
        selected_index[0] = '\0';
        selected_chat_id[0] = '\0';
    }

    puts("telegram text client: manual mode");
    tg_text_client_print_status(client_config);
    if (watch_seconds > 0UL) {
        printf("telegram text client: auto-read every %lu second(s)\n",
               watch_seconds);
    }
    rc = tg_text_client_poll_once(client_config);
    if (rc != 0) {
        return rc;
    }

    tg_text_client_print_help();
    for (;;) {
        if (selected_index[0] != '\0') {
            printf("tg:%s:%s> ",
                   tg_text_client_selected_label(selected_index),
                   selected_chat_id);
        } else {
            printf("tg> ");
        }
        fflush(stdout);
        if (watch_seconds > 0UL &&
            !tg_platform_stdin_readable(watch_seconds)) {
            puts("");
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (fgets(line, sizeof(line), stdin) == 0) {
            puts("telegram text client: eof");
            return 0;
        }
        tg_console_trim_ascii_space(line);
        if (line[0] == '\0') {
            continue;
        }
        if (strcmp(line, "q") == 0 ||
            strcmp(line, "quit") == 0 ||
            strcmp(line, "/quit") == 0) {
            puts("telegram text client: quit");
            return 0;
        }
        if (strcmp(line, "/back") == 0) {
            puts("telegram text client: already at top level");
            continue;
        }
        if (strcmp(line, "h") == 0 ||
            strcmp(line, "help") == 0 ||
            strcmp(line, "/help") == 0) {
            tg_text_client_print_help();
            continue;
        }
        if (strcmp(line, "s") == 0 ||
            strcmp(line, "status") == 0 ||
            strcmp(line, "/status") == 0) {
            tg_text_client_print_status(client_config);
            continue;
        }
        if (strcmp(line, "watch") == 0 || strcmp(line, "/watch") == 0) {
            if (watch_seconds == 0UL) {
                puts("telegram text client: auto-read disabled");
            } else {
                printf("telegram text client: auto-read every %lu second(s)\n",
                       watch_seconds);
            }
            continue;
        }
        if ((strncmp(line, "watch", 5) == 0 &&
             (line[5] == ' ' || line[5] == '\t')) ||
            (strncmp(line, "/watch", 6) == 0 &&
             (line[6] == ' ' || line[6] == '\t'))) {
            if (tg_text_client_set_watch_seconds(
                    line, &watch_seconds, "telegram text client") != 0) {
                return 2;
            }
            continue;
        }
        if (strcmp(line, "i") == 0 ||
            strcmp(line, "last") == 0 ||
            strcmp(line, "inbox") == 0 ||
            strcmp(line, "/last") == 0) {
            rc = tg_text_client_print_last_inbox_line(
                client_config->inbox_log_file_path);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strcmp(line, "l") == 0 ||
            strcmp(line, "list") == 0 ||
            strcmp(line, "chats") == 0 ||
            strcmp(line, "/chats") == 0) {
            rc = tg_text_client_print_chats(client_config->chat_state_file_path);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strcmp(line, "p") == 0 ||
            strcmp(line, "poll") == 0 ||
            strcmp(line, "read") == 0 ||
            strcmp(line, "refresh") == 0 ||
            strcmp(line, "/read") == 0 ||
            strcmp(line, "/refresh") == 0) {
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            rc = tg_text_client_print_chats(client_config->chat_state_file_path);
            if (rc != 0) {
                return rc;
            }
            continue;
        }

        index_is_ready = 0;
        if (strncmp(line, "chat", 4) == 0 &&
            (line[4] == ' ' || line[4] == '\t')) {
            if (tg_console_parse_index_command(line, 4, index_text,
                                               sizeof(index_text)) != 0) {
                puts("telegram text client: use chat <index>");
                continue;
            }
            index_is_ready = 1;
        } else if (strncmp(line, "open", 4) == 0 &&
                   (line[4] == ' ' || line[4] == '\t')) {
            if (tg_console_parse_index_command(line, 4, index_text,
                                               sizeof(index_text)) != 0) {
                puts("telegram text client: use open <index>");
                continue;
            }
            index_is_ready = 1;
        } else if (strncmp(line, "/open", 5) == 0 &&
                   (line[5] == ' ' || line[5] == '\t')) {
            if (tg_console_parse_index_command(line, 5, index_text,
                                               sizeof(index_text)) != 0) {
                puts("telegram text client: use /open <index>");
                continue;
            }
            index_is_ready = 1;
        } else if (tg_console_parse_index_command(line, 0, index_text,
                                                  sizeof(index_text)) == 0) {
            index_is_ready = 1;
        }
        if (index_is_ready) {
            if (tg_text_client_select_chat(client_config, index_text,
                                           selected_index,
                                           sizeof(selected_index),
                                           selected_chat_id,
                                           sizeof(selected_chat_id)) != 0) {
                continue;
            }
            rc = tg_text_client_chat_mode(client_config, selected_index,
                                          selected_chat_id);
            if (rc < 0) {
                return 2;
            }
            if (rc > 0) {
                puts("telegram text client: quit");
                return 0;
            }
            continue;
        }

        if (strncmp(line, "/send-id", 8) == 0 &&
            (line[8] == ' ' || line[8] == '\t')) {
            if (tg_text_client_parse_send_id_command(
                    line, 8, send_chat_id, sizeof(send_chat_id),
                    &send_id_text) != 0) {
                puts("telegram text client: use /send-id <chat-id> <text>");
                continue;
            }
            rc = tg_text_client_send_to_chat_id(client_config,
                                                send_chat_id,
                                                send_id_text, 0);
            if (rc != 0) {
                return rc;
            }
            if (tg_text_client_select_chat_id(client_config, send_chat_id,
                                              selected_index,
                                              sizeof(selected_index),
                                              selected_chat_id,
                                              sizeof(selected_chat_id)) != 0) {
                return 2;
            }
            continue;
        }
        if (strncmp(line, "/send", 5) == 0 &&
            (line[5] == ' ' || line[5] == '\t')) {
            send_text = tg_text_client_send_text_from_line(line, 5);
            if (send_text == 0) {
                puts("telegram text client: use /send <text>");
                continue;
            }
            if (selected_chat_id[0] == '\0') {
                puts("telegram text client: open a chat first with /open <index>");
                continue;
            }
            rc = tg_text_client_send_to_chat_id(client_config,
                                                selected_chat_id,
                                                send_text, 0);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (line[0] == 'r' &&
            (line[1] == ' ' || line[1] == '\t')) {
            rc = tg_text_client_handle_index_send(
                client_config, line, 1, "r <index> <text>");
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strncmp(line, "send", 4) == 0 &&
            (line[4] == ' ' || line[4] == '\t')) {
            rc = tg_text_client_handle_index_send(
                client_config, line, 4, "send <index> <text>");
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        if (strncmp(line, "reply", 5) == 0 &&
            (line[5] == ' ' || line[5] == '\t')) {
            rc = tg_text_client_handle_index_send(
                client_config, line, 5, "reply <index> <text>");
            if (rc != 0) {
                return rc;
            }
            continue;
        }

        puts("telegram text client: unknown command, use /help");
        tg_text_client_print_help();
    }
}

int tg_text_client_run_human(const tg_text_client_config *client_config)
{
    char line[512];
    char selected_index[32];
    char selected_chat_id[64];
    const char *poll_seconds_text;
    unsigned long watch_seconds;
    int rc;

    if (client_config == 0 || client_config->token_file_path == 0 ||
        client_config->offset_file_path == 0 ||
        client_config->chat_state_file_path == 0 ||
        client_config->selected_chat_file_path == 0) {
        return 2;
    }

    poll_seconds_text = client_config->poll_seconds_text;
    if (poll_seconds_text == 0) {
        poll_seconds_text = tg_text_client_human_poll_seconds_text;
    }
    watch_seconds = tg_text_client_initial_watch_seconds(poll_seconds_text,
                                                         5UL);
    selected_index[0] = '\0';
    selected_chat_id[0] = '\0';
    if (tg_text_client_load_selected_chat(client_config->selected_chat_file_path,
                                          selected_index,
                                          sizeof(selected_index),
                                          selected_chat_id,
                                          sizeof(selected_chat_id)) != 0) {
        selected_index[0] = '\0';
        selected_chat_id[0] = '\0';
    }

    puts(TG_APP_TITLE);
    puts("Type a message. Empty line checks for replies. Type quit to exit.");
    rc = tg_text_client_poll_once(client_config);
    if (rc != 0) {
        return rc;
    }
    if (selected_chat_id[0] == '\0') {
        (void)tg_text_client_select_first_chat_quiet(
            client_config, selected_index, sizeof(selected_index),
            selected_chat_id, sizeof(selected_chat_id));
    }

    for (;;) {
        if (watch_seconds > 0UL &&
            !tg_platform_stdin_readable(watch_seconds)) {
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            if (selected_chat_id[0] == '\0') {
                (void)tg_text_client_select_first_chat_quiet(
                    client_config, selected_index, sizeof(selected_index),
                    selected_chat_id, sizeof(selected_chat_id));
            }
            continue;
        }
        if (fgets(line, sizeof(line), stdin) == 0) {
            return 0;
        }
        tg_console_trim_ascii_space(line);
        if (line[0] == '\0') {
            rc = tg_text_client_poll_once(client_config);
            if (rc != 0) {
                return rc;
            }
            if (selected_chat_id[0] == '\0') {
                (void)tg_text_client_select_first_chat_quiet(
                    client_config, selected_index, sizeof(selected_index),
                    selected_chat_id, sizeof(selected_chat_id));
            }
            continue;
        }
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            puts("bye");
            return 0;
        }
        if (selected_chat_id[0] == '\0') {
            if (tg_text_client_is_chat_id_text(line) &&
                tg_text_client_select_chat_id_quiet(
                    client_config, line, selected_index,
                    sizeof(selected_index), selected_chat_id,
                    sizeof(selected_chat_id)) == 0) {
                puts("chat selected");
                continue;
            }
            if (tg_text_client_select_first_chat_quiet(
                    client_config, selected_index, sizeof(selected_index),
                    selected_chat_id, sizeof(selected_chat_id)) != 0) {
                puts("No chat yet. Send a Telegram message to the bot, press Enter, or type the chat id.");
                continue;
            }
        }

        rc = tg_text_client_send_to_chat_id(client_config, selected_chat_id,
                                            line, 0);
        if (rc != 0) {
            return rc;
        }
        rc = tg_text_client_poll_once(client_config);
        if (rc != 0) {
            return rc;
        }
    }
}

#if !defined(TG_NO_SELFTEST)
int tg_text_client_self_test(void)
{
    static const char chat_state_file_path[] =
        "telegram-text-client-self-test-chats.txt";
    static const char selected_file_path[] =
        "telegram-text-client-self-test-selected.txt";
    char line[256];
    char index_text[32];
    char chat_id[64];
    char direct_chat_id[64];
    char *message_text;
    tg_text_client_config client_config;
    unsigned long watch_seconds;

    remove(chat_state_file_path);
    remove(selected_file_path);
    memset(&client_config, 0, sizeof(client_config));
    client_config.selected_chat_file_path = selected_file_path;

    if (tg_client_state_build_line("111", "Alice", "2026-05-15",
                                   "First", line, sizeof(line)) != 0 ||
        tg_client_state_update_file_line(chat_state_file_path, "111",
                                         line, 0) != 0 ||
        tg_client_state_build_line("222", "Bob", "2026-05-15",
                                   "Second", line, sizeof(line)) != 0 ||
        tg_client_state_update_file_line(chat_state_file_path, "222",
                                         line, 0) != 0) {
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }

    strcpy(line, "/open 2");
    if (tg_console_parse_index_command(line, 5, index_text,
                                       sizeof(index_text)) != 0 ||
        strcmp(index_text, "2") != 0 ||
        tg_client_state_find_chat_id_by_index(chat_state_file_path, 2UL,
                                              chat_id, sizeof(chat_id)) != 0 ||
        strcmp(chat_id, "111") != 0) {
        puts("telegram text client self-test: open parse failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    if (tg_text_client_save_selected_chat(selected_file_path, index_text,
                                          chat_id) != 0 ||
        tg_text_client_load_selected_chat(selected_file_path, index_text,
                                          sizeof(index_text), chat_id,
                                          sizeof(chat_id)) != 0 ||
        strcmp(index_text, "2") != 0 || strcmp(chat_id, "111") != 0) {
        puts("telegram text client self-test: selected chat failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }

    strcpy(line, "/send Hello selected");
    if (tg_text_client_send_text_from_line(line, 5) == 0 ||
        strcmp(tg_text_client_send_text_from_line(line, 5),
               "Hello selected") != 0) {
        puts("telegram text client self-test: selected send parse failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    strcpy(line, "reply 2 Hello index");
    message_text = 0;
    if (tg_console_parse_reply_command(line, 5, index_text,
                                       sizeof(index_text),
                                       &message_text) != 0 ||
        strcmp(index_text, "2") != 0 ||
        message_text == 0 ||
        strcmp(message_text, "Hello index") != 0) {
        puts("telegram text client self-test: reply parse failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    strcpy(line, "/send-id -123 Hello direct");
    message_text = 0;
    if (tg_text_client_parse_send_id_command(line, 8, chat_id,
                                             sizeof(chat_id),
                                             &message_text) != 0 ||
        strcmp(chat_id, "-123") != 0 ||
        message_text == 0 ||
        strcmp(message_text, "Hello direct") != 0) {
        puts("telegram text client self-test: send-id parse failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    strcpy(line, "/send-id -123Hello direct");
    if (tg_text_client_parse_send_id_command(line, 8, chat_id,
                                             sizeof(chat_id),
                                             &message_text) == 0) {
        puts("telegram text client self-test: send-id separator failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    strcpy(direct_chat_id, chat_id);
    if (tg_text_client_select_chat_id(&client_config, direct_chat_id,
                                      index_text, sizeof(index_text),
                                      chat_id, sizeof(chat_id)) != 0 ||
        tg_text_client_load_selected_chat(selected_file_path, index_text,
                                          sizeof(index_text), chat_id,
                                          sizeof(chat_id)) != 0 ||
        strcmp(index_text, "0") != 0 || strcmp(chat_id, "-123") != 0 ||
        strcmp(tg_text_client_selected_label(index_text), "id") != 0) {
        puts("telegram text client self-test: direct chat selection failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    watch_seconds = 0UL;
    if (tg_console_parse_watch_command("/watch 3", &watch_seconds) != 0 ||
        watch_seconds != 3UL ||
        tg_console_parse_watch_command("/watch off", &watch_seconds) != 0 ||
        watch_seconds != 0UL) {
        puts("telegram text client self-test: watch parse failed");
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }
    if (tg_text_client_print_chats(chat_state_file_path) != 0) {
        remove(chat_state_file_path);
        remove(selected_file_path);
        return 2;
    }

    remove(chat_state_file_path);
    remove(selected_file_path);
    puts("telegram text client self-test: ok");
    return 0;
}
#endif /* !TG_NO_SELFTEST */
