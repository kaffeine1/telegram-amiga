/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_LOG_H
#define TG_LOG_H

/**
 * Logging severity. Lower numeric values are more severe.
 */
typedef enum tg_log_level {
    TG_LOG_ERROR = 0,
    TG_LOG_WARN = 1,
    TG_LOG_INFO = 2,
    TG_LOG_DEBUG = 3
} tg_log_level;

/**
 * Sets the minimum level emitted by tg_log().
 */
void tg_log_set_level(tg_log_level level);

/**
 * Returns the current minimum log level.
 */
tg_log_level tg_log_get_level(void);

/**
 * Emits message through the current platform logger if level is enabled.
 *
 * The message pointer is borrowed and must remain valid only for the duration
 * of the call.
 */
void tg_log(tg_log_level level, const char *message);

/**
 * Returns a static string for level. The caller must not free it.
 */
const char *tg_log_level_name(tg_log_level level);

#endif
