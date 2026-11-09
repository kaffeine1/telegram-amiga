/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tg_log.h"
#include "tg_platform.h"

static tg_log_level current_level = TG_LOG_INFO;

void tg_log_set_level(tg_log_level level)
{
    current_level = level;
}

tg_log_level tg_log_get_level(void)
{
    return current_level;
}

void tg_log(tg_log_level level, const char *message)
{
    if (level <= current_level) {
        tg_platform_log(tg_log_level_name(level), message);
    }
}

const char *tg_log_level_name(tg_log_level level)
{
    switch (level) {
    case TG_LOG_ERROR:
        return "error";
    case TG_LOG_WARN:
        return "warn";
    case TG_LOG_INFO:
        return "info";
    case TG_LOG_DEBUG:
        return "debug";
    default:
        return "unknown";
    }
}
