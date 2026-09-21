/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_FILE_H
#define TG_FILE_H

#include <stdio.h>

/**
 * File helper result.
 *
 * TOO_LARGE means the caller-owned destination buffer cannot hold the complete
 * file plus a final NUL terminator.
 */
typedef enum tg_file_status {
    TG_FILE_OK = 0,
    TG_FILE_INVALID_ARGUMENT = 1,
    TG_FILE_OPEN_FAILED = 2,
    TG_FILE_READ_FAILED = 3,
    TG_FILE_TOO_LARGE = 4,
    TG_FILE_WRITE_FAILED = 5
} tg_file_status;

/**
 * Reads a whole text file into caller-owned buffer.
 *
 * buffer receives a NUL-terminated byte string and text_length receives the byte
 * count excluding the terminator. The function does not allocate memory.
 */
tg_file_status tg_file_read_text(const char *path, char *buffer,
                                 unsigned long buffer_size,
                                 unsigned long *text_length);

/**
 * Writes a complete text buffer to a file.
 *
 * The input text does not need to be NUL-terminated because text_length is
 * explicit. Existing files are replaced.
 */
tg_file_status tg_file_write_text(const char *path, const char *text,
                                  unsigned long text_length);

/**
 * fopen() for a file the client is REPLACING: deletes it first, then creates
 * it. The same rule tg_file_write_text follows, for the same reason: on the
 * AROS FAT handler (issue 161) a file rewritten in place ends up empty, so
 * every save of a file that may already exist goes through here, with the
 * mode it would have given fopen(). Appends are fine and are not routed.
 */
FILE *tg_file_fopen_replace(const char *path, const char *mode);

/**
 * Appends a complete text buffer to a file.
 *
 * The file is created when missing. The input text does not need to be
 * NUL-terminated because text_length is explicit.
 */
tg_file_status tg_file_append_text(const char *path, const char *text,
                                   unsigned long text_length);

/**
 * Returns a static string for status. The caller must not free it.
 */
const char *tg_file_status_name(tg_file_status status);

#endif
