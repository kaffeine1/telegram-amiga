/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <string.h>

#include "tg_console.h"

void tg_console_trim_ascii_space(char *text)
{
    unsigned long start;
    unsigned long end;
    unsigned long length;

    if (text == 0) {
        return;
    }

    start = 0;
    while (text[start] == ' ' || text[start] == '\t' ||
           text[start] == '\r' || text[start] == '\n') {
        ++start;
    }

    end = (unsigned long)strlen(text);
    while (end > start &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }

    length = end - start;
    if (start > 0 && length > 0) {
        memmove(text, text + start, length);
    }
    text[length] = '\0';
}

int tg_console_parse_reply_command(char *line, unsigned long command_length,
                                   char *index_buffer,
                                   unsigned long index_buffer_size,
                                   char **text_start)
{
    char *cursor;
    char *index_start;
    unsigned long index_length;

    if (line == 0 || index_buffer == 0 || index_buffer_size == 0 ||
        text_start == 0) {
        return 1;
    }

    cursor = line + command_length;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    index_start = cursor;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
    }
    index_length = (unsigned long)(cursor - index_start);
    if (index_length == 0 || index_length + 1 > index_buffer_size) {
        return 1;
    }
    memcpy(index_buffer, index_start, index_length);
    index_buffer[index_length] = '\0';

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return 1;
    }
    *text_start = cursor;
    return 0;
}

int tg_console_parse_index_command(char *line, unsigned long command_length,
                                   char *index_buffer,
                                   unsigned long index_buffer_size)
{
    char *cursor;
    char *index_start;
    unsigned long index_length;

    if (line == 0 || index_buffer == 0 || index_buffer_size == 0) {
        return 1;
    }

    cursor = line + command_length;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    index_start = cursor;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
    }
    index_length = (unsigned long)(cursor - index_start);
    if (index_length == 0 || index_length + 1 > index_buffer_size) {
        return 1;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor != '\0') {
        return 1;
    }

    memcpy(index_buffer, index_start, index_length);
    index_buffer[index_length] = '\0';
    return 0;
}

static int tg_console_parse_decimal_ulong(const char *text,
                                          unsigned long *value)
{
    const char *cursor;
    unsigned long result;

    if (text == 0 || value == 0 || text[0] == '\0') {
        return 1;
    }

    cursor = text;
    result = 0UL;
    while (*cursor >= '0' && *cursor <= '9') {
        unsigned long digit;

        digit = (unsigned long)(*cursor - '0');
        if (result > (ULONG_MAX - digit) / 10UL) {
            return 1;
        }
        result = result * 10UL + digit;
        ++cursor;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor != '\0') {
        return 1;
    }

    *value = result;
    return 0;
}

int tg_console_parse_watch_command(const char *line,
                                   unsigned long *watch_seconds)
{
    const char *cursor;
    unsigned long prefix_length;
    unsigned long seconds;

    if (line == 0 || watch_seconds == 0) {
        return 1;
    }
    if (strncmp(line, "/watch", 6) == 0 &&
        (line[6] == '\0' || line[6] == ' ' || line[6] == '\t')) {
        prefix_length = 6UL;
    } else if (strncmp(line, "watch", 5) == 0 &&
               (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
        prefix_length = 5UL;
    } else {
        return 1;
    }

    cursor = line + prefix_length;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (strcmp(cursor, "off") == 0) {
        *watch_seconds = 0UL;
        return 0;
    }
    if (tg_console_parse_decimal_ulong(cursor, &seconds) != 0 ||
        seconds > 3600UL) {
        return 1;
    }

    *watch_seconds = seconds;
    return 0;
}
