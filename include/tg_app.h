/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_APP_H
#define TG_APP_H

/**
 * Runs the command-line bootstrap application.
 *
 * The function does not take ownership of argc/argv strings. It returns 0 on
 * success, 1 for command-line usage errors and 2 for runtime/test failures.
 */
int tg_app_run(int argc, char **argv);

#endif
