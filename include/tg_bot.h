/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_BOT_H
#define TG_BOT_H

#include "tg_https.h"
#include "tg_telegram.h"

/**
 * Bot API orchestration result.
 *
 * TELEGRAM_ERROR means the HTTP response was received but could not be parsed as
 * a Telegram response; telegram_status contains the lower-level reason.
 * HTTPS_ERROR means the live transport failed before a parseable HTTP response
 * was available. BODY_ERROR means a request JSON body could not be built in the
 * caller-provided or internal buffer.
 */
typedef enum tg_bot_status {
    TG_BOT_OK = 0,
    TG_BOT_INVALID_ARGUMENT = 1,
    TG_BOT_TOKEN_ERROR = 2,
    TG_BOT_PATH_ERROR = 3,
    TG_BOT_HTTPS_ERROR = 4,
    TG_BOT_TELEGRAM_ERROR = 5,
    TG_BOT_BODY_ERROR = 6,
    TG_BOT_UPDATE_ERROR = 7
} tg_bot_status;

/**
 * Detail outputs produced by tg_bot_get_me_from_token_file().
 *
 * The status fields are valid only when the corresponding tg_bot_status points
 * to that layer. response contains borrowed views into the caller-owned HTTP
 * buffer passed to tg_bot_get_me_from_token_file().
 */
typedef struct tg_bot_call_result {
    tg_file_status file_status;
    tg_telegram_status telegram_status;
    tg_https_status https_status;
    tg_tls_status tls_status;
    tg_net_status net_status;
    tg_http_parse_status http_parse_status;
    tg_json_status json_status;
    tg_telegram_http_response response;
} tg_bot_call_result;

/**
 * Minimal message kind detected inside one Telegram update.
 */
typedef enum tg_bot_message_kind {
    TG_BOT_MESSAGE_NONE = 0,
    TG_BOT_MESSAGE_TEXT = 1,
    TG_BOT_MESSAGE_COMMAND = 2,
    TG_BOT_MESSAGE_PHOTO = 3,
    TG_BOT_MESSAGE_STICKER = 4,
    TG_BOT_MESSAGE_DOCUMENT = 5,
    TG_BOT_MESSAGE_UNSUPPORTED = 6
} tg_bot_message_kind;

/**
 * Minimal summary of one Telegram update.
 *
 * update_id and chat_id are copied into fixed caller-visible buffers because
 * callers commonly need to store/print them. sender_name is decoded copied
 * text from message.from.username or message.from.first_name when present. date
 * is copied as the raw Telegram Unix timestamp when present. text is a borrowed
 * raw JSON string view into the HTTP response buffer; call tg_json_string_decode()
 * before presenting or reusing it as user-visible text.
 */
typedef struct tg_bot_update_summary {
    int has_update;
    int has_message;
    int has_date;
    int has_sender_name;
    int has_text;
    tg_bot_message_kind message_kind;
    char update_id[32];
    char chat_id[32];
    char date[32];
    char sender_name[128];
    const char *text;
    unsigned long text_length;
} tg_bot_update_summary;

/**
 * Initializes a call result to default OK/empty values.
 */
void tg_bot_call_result_init(tg_bot_call_result *result);

/**
 * Initializes an update summary to empty values.
 */
void tg_bot_update_summary_init(tg_bot_update_summary *update);

/**
 * Parses a complete HTTP response as the result of Telegram getMe.
 *
 * This is the offline-testable half of the Bot API layer. http_response remains
 * owned by the caller and must stay valid while result->response is used.
 */
tg_bot_status tg_bot_parse_get_me_http_response(const char *http_response,
                                                unsigned long http_response_length,
                                                tg_bot_call_result *result);

/**
 * Parses a complete HTTP response as the result of Telegram getUpdates.
 *
 * result->response.api.result is typically an array and remains a borrowed view
 * into http_response. Use tg_bot_get_updates_at() to extract individual update
 * summaries from that array.
 */
tg_bot_status tg_bot_parse_get_updates_http_response(const char *http_response,
                                                     unsigned long http_response_length,
                                                     tg_bot_call_result *result);

/**
 * Extracts the first update summary from a parsed getUpdates response.
 *
 * response must come from tg_bot_parse_get_updates_http_response() or a live
 * getUpdates call. Empty result arrays return TG_BOT_OK with has_update == 0.
 */
tg_bot_status tg_bot_get_updates_first(const tg_bot_call_result *result,
                                       tg_bot_update_summary *update);

/**
 * Extracts one update summary from a parsed getUpdates response by index.
 *
 * Empty arrays or out-of-range indexes return TG_BOT_OK with has_update == 0.
 * The text view remains borrowed from the original HTTP response buffer.
 */
tg_bot_status tg_bot_get_updates_at(const tg_bot_call_result *result,
                                    unsigned long index,
                                    tg_bot_update_summary *update);

/**
 * Builds the next getUpdates offset from update->update_id.
 *
 * next_offset receives update_id + 1 as decimal text. Returns BODY_ERROR if the
 * caller-owned buffer cannot hold the result.
 */
tg_bot_status tg_bot_update_next_offset(const tg_bot_update_summary *update,
                                        char *next_offset,
                                        unsigned long next_offset_size);

/**
 * Returns a static string for a message kind. The caller must not free it.
 */
const char *tg_bot_message_kind_name(tg_bot_message_kind kind);

/**
 * Parses a complete HTTP response as the result of Telegram sendMessage.
 *
 * This function does not allocate memory. result->response contains borrowed
 * views into http_response, so the caller must keep http_response alive while
 * using result.
 */
tg_bot_status tg_bot_parse_send_message_http_response(const char *http_response,
                                                      unsigned long http_response_length,
                                                      tg_bot_call_result *result);

/**
 * Builds a JSON body for Telegram sendMessage.
 *
 * chat_id and text are encoded as JSON strings, so numeric chat ids and
 * @channel names both use the same path. body_buffer is caller-owned and
 * receives a NUL-terminated JSON object. Returns TG_BOT_BODY_ERROR if the
 * buffer is too small.
 */
tg_bot_status tg_bot_build_send_message_body(const char *chat_id, const char *text,
                                             char *body_buffer,
                                             unsigned long body_buffer_size,
                                             unsigned long *body_length);

/**
 * Loads a token from file and performs getMe using HTTPS GET.
 *
 * http_buffer is caller-owned and receives the raw HTTP response. This function
 * does not print or expose the token. Live TLS behavior depends on the platform
 * backend and should be tested carefully on MorphOS.
 */
tg_bot_status tg_bot_get_me_from_token_file(const char *token_file_path,
                                            char *http_buffer,
                                            unsigned long http_buffer_size,
                                            unsigned long *http_response_length,
                                            tg_bot_call_result *result,
                                            char *error_buffer,
                                            unsigned long error_buffer_size);

/**
 * Loads a token from file and performs getUpdates using HTTPS GET.
 *
 * http_buffer is caller-owned and receives the raw HTTP response. The token and
 * generated request path are never printed by this function. The returned
 * Telegram result is currently exposed as a raw borrowed JSON array.
 */
tg_bot_status tg_bot_get_updates_from_token_file(const char *token_file_path,
                                                 char *http_buffer,
                                                 unsigned long http_buffer_size,
                                                 unsigned long *http_response_length,
                                                 tg_bot_call_result *result,
                                                 char *error_buffer,
                                                 unsigned long error_buffer_size);

/**
 * Loads a token from file and performs getUpdates with an optional offset.
 *
 * offset may be NULL or empty. When present, it must contain only decimal
 * digits and is appended as getUpdates?offset=<offset>.
 */
tg_bot_status tg_bot_get_updates_from_token_file_with_offset(
    const char *token_file_path,
    const char *offset,
    char *http_buffer,
    unsigned long http_buffer_size,
    unsigned long *http_response_length,
    tg_bot_call_result *result,
    char *error_buffer,
    unsigned long error_buffer_size);

/**
 * Loads a token from file and performs sendMessage using HTTPS POST.
 *
 * The token and generated request path are never printed by this function.
 * chat_id and text are copied into an internal JSON body buffer with JSON string
 * escaping. http_buffer is caller-owned and receives the raw HTTP response.
 */
tg_bot_status tg_bot_send_message_from_token_file(const char *token_file_path,
                                                  const char *chat_id,
                                                  const char *text,
                                                  char *http_buffer,
                                                  unsigned long http_buffer_size,
                                                  unsigned long *http_response_length,
                                                  tg_bot_call_result *result,
                                                  char *error_buffer,
                                                  unsigned long error_buffer_size);

/**
 * Returns a static string for status. The caller must not free it.
 */
const char *tg_bot_status_name(tg_bot_status status);

#endif
