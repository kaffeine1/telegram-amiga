/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "tg_client_state.h"
#include "tg_file.h"

static int tg_client_state_append_char(char *buffer,
                                       unsigned long buffer_size,
                                       unsigned long *position,
                                       char c)
{
    if (*position + 1 >= buffer_size) {
        return 1;
    }
    buffer[*position] = c;
    ++(*position);
    buffer[*position] = '\0';
    return 0;
}

static int tg_client_state_append_text(char *buffer,
                                       unsigned long buffer_size,
                                       unsigned long *position,
                                       const char *text)
{
    while (*text != '\0') {
        if (tg_client_state_append_char(buffer, buffer_size,
                                        position, *text) != 0) {
            return 1;
        }
        ++text;
    }
    return 0;
}

static int tg_client_state_append_n(char *buffer,
                                    unsigned long buffer_size,
                                    unsigned long *position,
                                    const char *text,
                                    unsigned long text_length)
{
    unsigned long index;

    for (index = 0; index < text_length; ++index) {
        if (tg_client_state_append_char(buffer, buffer_size,
                                        position, text[index]) != 0) {
            return 1;
        }
    }
    return 0;
}

static void tg_client_state_sanitize_field(char *text)
{
    while (*text != '\0') {
        if (*text == '\r' || *text == '\n' || *text == '\t' || *text == '|') {
            *text = ' ';
        }
        ++text;
    }
}

static int tg_client_state_copy_sanitized(const char *source,
                                          char *buffer,
                                          unsigned long buffer_size)
{
    unsigned long length;

    if (source == 0 || buffer == 0 || buffer_size == 0) {
        return 1;
    }
    length = (unsigned long)strlen(source);
    if (length + 1 > buffer_size) {
        return 1;
    }
    strcpy(buffer, source);
    tg_client_state_sanitize_field(buffer);
    return 0;
}

int tg_client_state_build_line(const char *chat_id,
                               const char *sender,
                               const char *date,
                               const char *text,
                               char *buffer,
                               unsigned long buffer_size)
{
    char sender_text[128];
    char date_text[64];
    char content_text[512];
    unsigned long position;

    if (chat_id == 0 || chat_id[0] == '\0' ||
        sender == 0 || date == 0 || text == 0 ||
        buffer == 0 || buffer_size == 0) {
        return 1;
    }

    if (tg_client_state_copy_sanitized(sender, sender_text,
                                       sizeof(sender_text)) != 0 ||
        tg_client_state_copy_sanitized(date, date_text,
                                       sizeof(date_text)) != 0 ||
        tg_client_state_copy_sanitized(text, content_text,
                                       sizeof(content_text)) != 0) {
        return 1;
    }

    position = 0;
    buffer[0] = '\0';
    if (tg_client_state_append_text(buffer, buffer_size, &position,
                                    chat_id) != 0 ||
        tg_client_state_append_char(buffer, buffer_size, &position, '|') != 0 ||
        tg_client_state_append_text(buffer, buffer_size, &position,
                                    sender_text) != 0 ||
        tg_client_state_append_char(buffer, buffer_size, &position, '|') != 0 ||
        tg_client_state_append_text(buffer, buffer_size, &position,
                                    date_text) != 0 ||
        tg_client_state_append_char(buffer, buffer_size, &position, '|') != 0 ||
        tg_client_state_append_text(buffer, buffer_size, &position,
                                    content_text) != 0 ||
        tg_client_state_append_char(buffer, buffer_size, &position, '\n') != 0) {
        return 1;
    }

    return 0;
}

static int tg_client_state_line_matches(const char *line,
                                        unsigned long line_length,
                                        const char *chat_id)
{
    unsigned long chat_id_length;

    if (line == 0 || chat_id == 0) {
        return 0;
    }
    chat_id_length = (unsigned long)strlen(chat_id);
    return line_length > chat_id_length &&
           strncmp(line, chat_id, chat_id_length) == 0 &&
           line[chat_id_length] == '|';
}

int tg_client_state_update_file_line(const char *path,
                                     const char *chat_id,
                                     const char *new_line,
                                     int verbose)
{
    tg_file_status file_status;
    char current[16384];
    char rewritten[16384];
    unsigned long current_length;
    unsigned long input_pos;
    unsigned long output_pos;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    if (chat_id == 0 || chat_id[0] == '\0' ||
        new_line == 0 || new_line[0] == '\0') {
        return 2;
    }

    current[0] = '\0';
    current_length = 0;
    file_status = tg_file_read_text(path, current, sizeof(current),
                                    &current_length);
    if (file_status != TG_FILE_OK && file_status != TG_FILE_OPEN_FAILED) {
        printf("chat state: read failed: %s\n",
               tg_file_status_name(file_status));
        return 2;
    }

    rewritten[0] = '\0';
    input_pos = 0;
    output_pos = 0;
    if (tg_client_state_append_text(rewritten, sizeof(rewritten), &output_pos,
                                    new_line) != 0) {
        puts("chat state: file too large");
        return 2;
    }
    while (input_pos < current_length) {
        unsigned long line_start;
        unsigned long line_end;
        unsigned long line_length;

        line_start = input_pos;
        line_end = line_start;
        while (line_end < current_length &&
               current[line_end] != '\n' &&
               current[line_end] != '\r') {
            ++line_end;
        }
        line_length = line_end - line_start;

        if (tg_client_state_line_matches(current + line_start, line_length,
                                         chat_id)) {
            /* The newest update is already written first; skip the old row. */
        } else if (line_length > 0) {
            if (tg_client_state_append_n(rewritten, sizeof(rewritten),
                                         &output_pos, current + line_start,
                                         line_length) != 0 ||
                tg_client_state_append_char(rewritten, sizeof(rewritten),
                                            &output_pos, '\n') != 0) {
                puts("chat state: file too large");
                return 2;
            }
        }

        input_pos = line_end;
        while (input_pos < current_length &&
               (current[input_pos] == '\n' || current[input_pos] == '\r')) {
            ++input_pos;
        }
    }

    file_status = tg_file_write_text(path, rewritten, output_pos);
    if (file_status != TG_FILE_OK) {
        printf("chat state: write failed: %s\n",
               tg_file_status_name(file_status));
        return 2;
    }

    if (verbose) {
        printf("chat state updated: %s\n", path);
    }
    return 0;
}

static int tg_client_state_copy_field(const char *line,
                                      unsigned long line_length,
                                      unsigned long *position,
                                      char *buffer,
                                      unsigned long buffer_size)
{
    unsigned long output_pos;

    if (line == 0 || position == 0 || buffer == 0 || buffer_size == 0) {
        return 1;
    }

    output_pos = 0;
    buffer[0] = '\0';
    while (*position < line_length && line[*position] != '|') {
        if (output_pos + 1 >= buffer_size) {
            return 1;
        }
        buffer[output_pos] = line[*position];
        ++output_pos;
        ++(*position);
    }
    buffer[output_pos] = '\0';
    if (*position < line_length && line[*position] == '|') {
        ++(*position);
    }
    return 0;
}

int tg_client_state_parse_line(const char *line,
                               unsigned long line_length,
                               char *chat_id,
                               unsigned long chat_id_size,
                               char *sender,
                               unsigned long sender_size,
                               char *date,
                               unsigned long date_size,
                               char *text,
                               unsigned long text_size)
{
    unsigned long position;

    position = 0;
    if (tg_client_state_copy_field(line, line_length, &position,
                                   chat_id, chat_id_size) != 0 ||
        tg_client_state_copy_field(line, line_length, &position,
                                   sender, sender_size) != 0 ||
        tg_client_state_copy_field(line, line_length, &position,
                                   date, date_size) != 0 ||
        tg_client_state_copy_field(line, line_length, &position,
                                   text, text_size) != 0) {
        return 1;
    }
    return chat_id[0] == '\0' ? 1 : 0;
}

int tg_client_state_for_each_line(const char *path,
                                  tg_client_state_line_callback callback,
                                  void *context,
                                  int missing_ok)
{
    tg_file_status file_status;
    char content[16384];
    unsigned long content_length;
    unsigned long input_pos;
    unsigned long index;

    if (path == 0 || path[0] == '\0' || callback == 0) {
        return 2;
    }

    file_status = tg_file_read_text(path, content, sizeof(content),
                                    &content_length);
    if (file_status != TG_FILE_OK) {
        if (missing_ok && file_status == TG_FILE_OPEN_FAILED) {
            puts("telegram chats: none");
            return 0;
        }
        printf("telegram chats: read failed: %s\n",
               tg_file_status_name(file_status));
        return 2;
    }

    input_pos = 0;
    index = 1UL;
    while (input_pos < content_length) {
        unsigned long line_start;
        unsigned long line_end;
        unsigned long line_length;

        line_start = input_pos;
        line_end = line_start;
        while (line_end < content_length &&
               content[line_end] != '\n' &&
               content[line_end] != '\r') {
            ++line_end;
        }
        line_length = line_end - line_start;
        if (line_length > 0) {
            if (callback(index, content + line_start, line_length,
                         context) != 0) {
                return 2;
            }
            ++index;
        }

        input_pos = line_end;
        while (input_pos < content_length &&
               (content[input_pos] == '\n' || content[input_pos] == '\r')) {
            ++input_pos;
        }
    }

    if (index == 1UL) {
        puts("telegram chats: none");
    }
    return 0;
}

typedef struct tg_client_state_find_context {
    unsigned long wanted_index;
    int found;
    char *chat_id;
    unsigned long chat_id_size;
} tg_client_state_find_context;

static int tg_client_state_find_line_callback(unsigned long index,
                                             const char *line,
                                             unsigned long line_length,
                                             void *context)
{
    tg_client_state_find_context *find_context;
    char chat_id[64];
    char sender[128];
    char date[64];
    char text[512];

    find_context = (tg_client_state_find_context *)context;
    if (find_context == 0 || index != find_context->wanted_index) {
        return 0;
    }
    if (tg_client_state_parse_line(line, line_length,
                                   chat_id, sizeof(chat_id),
                                   sender, sizeof(sender),
                                   date, sizeof(date),
                                   text, sizeof(text)) != 0) {
        puts("telegram chats: selected line is invalid");
        return 1;
    }
    if (strlen(chat_id) + 1 > find_context->chat_id_size) {
        puts("telegram chats: selected chat id too long");
        return 1;
    }
    strcpy(find_context->chat_id, chat_id);
    find_context->found = 1;
    return 0;
}

int tg_client_state_find_chat_id_by_index(const char *path,
                                          unsigned long wanted_index,
                                          char *chat_id,
                                          unsigned long chat_id_size)
{
    tg_client_state_find_context context;
    int rc;

    if (wanted_index == 0 || chat_id == 0 || chat_id_size == 0) {
        return 2;
    }

    chat_id[0] = '\0';
    context.wanted_index = wanted_index;
    context.found = 0;
    context.chat_id = chat_id;
    context.chat_id_size = chat_id_size;

    rc = tg_client_state_for_each_line(path,
                                       tg_client_state_find_line_callback,
                                       &context, 0);
    if (rc != 0) {
        return rc;
    }
    if (!context.found) {
        printf("telegram chats: index not found: %lu\n", wanted_index);
        return 2;
    }
    return 0;
}
