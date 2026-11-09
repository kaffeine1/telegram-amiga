/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "tg_file.h"
#include "tg_offset_state.h"

static int tg_offset_state_is_decimal_text(const char *text)
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

static void tg_offset_state_trim_ascii_space(char *text)
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

int tg_offset_state_load_file(const char *path,
                              char *offset,
                              unsigned long offset_size)
{
    tg_file_status file_status;
    unsigned long offset_length;

    if (offset == 0 || offset_size == 0) {
        return 1;
    }
    offset[0] = '\0';

    file_status = tg_file_read_text(path, offset, offset_size, &offset_length);
    if (file_status == TG_FILE_OPEN_FAILED) {
        return 0;
    }
    if (file_status != TG_FILE_OK) {
        printf("telegram offset file: failed: %s\n",
               tg_file_status_name(file_status));
        return 1;
    }

    tg_offset_state_trim_ascii_space(offset);
    if (offset[0] != '\0' && !tg_offset_state_is_decimal_text(offset)) {
        puts("telegram offset file: invalid offset");
        return 1;
    }

    return 0;
}

int tg_offset_state_save_file_mode(const char *path, const char *offset,
                                   int verbose)
{
    tg_file_status file_status;
    char line[40];
    unsigned long offset_length;

    if (path == 0 || offset == 0 ||
        !tg_offset_state_is_decimal_text(offset)) {
        return 1;
    }

    offset_length = (unsigned long)strlen(offset);
    if (offset_length + 2 > sizeof(line)) {
        return 1;
    }

    strcpy(line, offset);
    line[offset_length] = '\n';
    line[offset_length + 1] = '\0';

    file_status = tg_file_write_text(path, line, offset_length + 1);
    if (file_status != TG_FILE_OK) {
        printf("telegram offset file: write failed: %s\n",
               tg_file_status_name(file_status));
        return 1;
    }

    if (verbose) {
        printf("telegram offset saved: %s\n", offset);
    }
    return 0;
}

int tg_offset_state_save_file(const char *path, const char *offset)
{
    return tg_offset_state_save_file_mode(path, offset, 1);
}
