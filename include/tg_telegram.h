/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_TELEGRAM_H
#define TG_TELEGRAM_H

#include "tg_file.h"
#include "tg_http.h"
#include "tg_json.h"

/**
 * Telegram API helper result.
 *
 * JSON_ERROR means the underlying JSON parser failed and json_status contains
 * the parser reason. MISSING_OK means the response was JSON but did not include
 * Telegram's required top-level "ok" field. TYPE_MISMATCH means a known field
 * exists but has an unexpected JSON type. HTTP_PARSE_ERROR means the input was
 * not a complete valid HTTP response.
 */
typedef enum tg_telegram_status {
    TG_TELEGRAM_OK = 0,
    TG_TELEGRAM_INVALID_ARGUMENT = 1,
    TG_TELEGRAM_JSON_ERROR = 2,
    TG_TELEGRAM_MISSING_OK = 3,
    TG_TELEGRAM_TYPE_MISMATCH = 4,
    TG_TELEGRAM_BUFFER_TOO_SMALL = 5,
    TG_TELEGRAM_HTTP_PARSE_ERROR = 6,
    TG_TELEGRAM_FILE_ERROR = 7
} tg_telegram_status;

/**
 * Parsed Telegram API response envelope.
 *
 * description and result are borrowed views into the caller-owned JSON buffer
 * supplied to tg_telegram_parse_response(); no allocation is done.
 */
typedef struct tg_telegram_response {
    int ok;
    int has_description;
    const char *description;
    unsigned long description_length;
    int has_result;
    tg_json_value result;
} tg_telegram_response;

/**
 * Parsed Telegram response carried inside an HTTP response.
 *
 * api contains borrowed views into the caller-owned HTTP response buffer,
 * because the JSON body is part of that same buffer. No allocation is done.
 */
typedef struct tg_telegram_http_response {
    int http_status_code;
    tg_telegram_response api;
} tg_telegram_http_response;

/**
 * Returns the canonical Bot API host, currently "api.telegram.org".
 *
 * The returned string is static and must not be freed.
 */
const char *tg_telegram_api_host(void);

/**
 * Builds the Bot API request path for a token and method.
 *
 * path_buffer is caller-owned and receives a NUL-terminated path in the form
 * /bot<token>/<method>. path_length receives the byte count excluding the NUL.
 * Returns TG_TELEGRAM_BUFFER_TOO_SMALL if path_buffer_size is insufficient.
 *
 * Token values are treated as opaque strings and are not validated here. Do not
 * log or commit real bot tokens.
 */
tg_telegram_status tg_telegram_build_bot_path(const char *token, const char *method,
                                              char *path_buffer,
                                              unsigned long path_buffer_size,
                                              unsigned long *path_length);

/**
 * Loads a bot token from a text file into caller-owned token_buffer.
 *
 * Leading and trailing spaces, tabs, CR and LF are removed. The resulting token
 * is NUL-terminated and token_length excludes the terminator. The function does
 * not allocate memory. file_status is optional and receives details when the
 * return value is TG_TELEGRAM_FILE_ERROR.
 */
tg_telegram_status tg_telegram_load_token_file(const char *path, char *token_buffer,
                                               unsigned long token_buffer_size,
                                               unsigned long *token_length,
                                               tg_file_status *file_status);

/**
 * Parses Telegram's top-level JSON response envelope.
 *
 * The input JSON buffer remains owned by the caller and must stay valid while
 * response is used. json_status is optional and receives details when the return
 * value is TG_TELEGRAM_JSON_ERROR or TG_TELEGRAM_MISSING_OK.
 */
tg_telegram_status tg_telegram_parse_response(const char *json, unsigned long json_length,
                                               tg_telegram_response *response,
                                               tg_json_status *json_status);

/**
 * Parses a complete HTTP response and then parses its body as Telegram JSON.
 *
 * The input buffer remains owned by the caller and must stay valid while
 * response is used. Non-2xx HTTP status codes are not treated as parser errors;
 * callers should inspect response->http_status_code and response->api.ok.
 * http_parse_status and json_status are optional detail outputs.
 */
tg_telegram_status tg_telegram_parse_http_response(const char *http_response,
                                                   unsigned long http_response_length,
                                                   tg_telegram_http_response *response,
                                                   tg_http_parse_status *http_parse_status,
                                                   tg_json_status *json_status);

/**
 * Returns a static string for status. The caller must not free it.
 */
const char *tg_telegram_status_name(tg_telegram_status status);

#endif
