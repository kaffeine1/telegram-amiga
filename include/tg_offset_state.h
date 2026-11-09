/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_OFFSET_STATE_H
#define TG_OFFSET_STATE_H

/**
 * Loads a persisted Telegram update offset from path.
 *
 * Missing files are treated as empty offset state. offset receives a
 * NUL-terminated decimal string or an empty string. Returns 0 on success.
 */
int tg_offset_state_load_file(const char *path,
                              char *offset,
                              unsigned long offset_size);

/**
 * Saves a decimal Telegram update offset to path.
 *
 * The file is replaced and the offset is written with a trailing newline.
 * Returns 0 on success.
 */
int tg_offset_state_save_file(const char *path, const char *offset);
int tg_offset_state_save_file_mode(const char *path, const char *offset,
                                   int verbose);

#endif
