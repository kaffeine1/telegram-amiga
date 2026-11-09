/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_bot.h"

static tg_bot_status tg_bot_parse_http_response(const char *http_response,
                                                unsigned long http_response_length,
                                                tg_bot_call_result *result)
{
    tg_telegram_status telegram_status;

    if (http_response == 0 || result == 0) {
        return TG_BOT_INVALID_ARGUMENT;
    }

    tg_bot_call_result_init(result);
    telegram_status = tg_telegram_parse_http_response(http_response, http_response_length,
                                                      &result->response,
                                                      &result->http_parse_status,
                                                      &result->json_status);
    result->telegram_status = telegram_status;
    if (telegram_status != TG_TELEGRAM_OK) {
        return TG_BOT_TELEGRAM_ERROR;
    }

    return TG_BOT_OK;
}

static tg_bot_status tg_bot_append_char(char *buffer, unsigned long buffer_size,
                                        unsigned long *position, char c)
{
    if (*position + 1 >= buffer_size) {
        return TG_BOT_BODY_ERROR;
    }
    buffer[*position] = c;
    ++(*position);
    buffer[*position] = '\0';
    return TG_BOT_OK;
}

static tg_bot_status tg_bot_append_text(char *buffer, unsigned long buffer_size,
                                        unsigned long *position, const char *text)
{
    while (*text != '\0') {
        if (tg_bot_append_char(buffer, buffer_size, position, *text) != TG_BOT_OK) {
            return TG_BOT_BODY_ERROR;
        }
        ++text;
    }
    return TG_BOT_OK;
}

static int tg_bot_is_decimal_text(const char *text)
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

static int tg_bot_raw_json_string_starts_with(const tg_json_value *value, char c)
{
    return value != 0 && value->type == TG_JSON_VALUE_STRING &&
           value->length > 0 && value->start != 0 && value->start[0] == c;
}

static tg_bot_status tg_bot_append_json_hex_escape(char *buffer,
                                                   unsigned long buffer_size,
                                                   unsigned long *position,
                                                   unsigned char c)
{
    static const char hex[] = "0123456789abcdef";

    if (tg_bot_append_text(buffer, buffer_size, position, "\\u00") != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }
    if (tg_bot_append_char(buffer, buffer_size, position, hex[(c >> 4) & 0x0f]) != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }
    return tg_bot_append_char(buffer, buffer_size, position, hex[c & 0x0f]);
}

static tg_bot_status tg_bot_append_json_string(char *buffer, unsigned long buffer_size,
                                               unsigned long *position, const char *text)
{
    unsigned char c;

    if (tg_bot_append_char(buffer, buffer_size, position, '"') != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }

    while (*text != '\0') {
        c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            if (tg_bot_append_char(buffer, buffer_size, position, '\\') != TG_BOT_OK ||
                tg_bot_append_char(buffer, buffer_size, position, (char)c) != TG_BOT_OK) {
                return TG_BOT_BODY_ERROR;
            }
        } else if (c == '\n') {
            if (tg_bot_append_text(buffer, buffer_size, position, "\\n") != TG_BOT_OK) {
                return TG_BOT_BODY_ERROR;
            }
        } else if (c == '\r') {
            if (tg_bot_append_text(buffer, buffer_size, position, "\\r") != TG_BOT_OK) {
                return TG_BOT_BODY_ERROR;
            }
        } else if (c == '\t') {
            if (tg_bot_append_text(buffer, buffer_size, position, "\\t") != TG_BOT_OK) {
                return TG_BOT_BODY_ERROR;
            }
        } else if (c < 0x20) {
            if (tg_bot_append_json_hex_escape(buffer, buffer_size, position, c) != TG_BOT_OK) {
                return TG_BOT_BODY_ERROR;
            }
        } else {
            if (tg_bot_append_char(buffer, buffer_size, position, (char)c) != TG_BOT_OK) {
                return TG_BOT_BODY_ERROR;
            }
        }
        ++text;
    }

    return tg_bot_append_char(buffer, buffer_size, position, '"');
}

void tg_bot_call_result_init(tg_bot_call_result *result)
{
    if (result == 0) {
        return;
    }
    result->file_status = TG_FILE_OK;
    result->telegram_status = TG_TELEGRAM_OK;
    result->https_status = TG_HTTPS_OK;
    result->tls_status = TG_TLS_OK;
    result->net_status = TG_NET_OK;
    result->http_parse_status = TG_HTTP_PARSE_OK;
    result->json_status = TG_JSON_OK;
    result->response.http_status_code = 0;
    result->response.api.ok = 0;
    result->response.api.has_description = 0;
    result->response.api.description = 0;
    result->response.api.description_length = 0;
    result->response.api.has_result = 0;
    result->response.api.result.type = TG_JSON_VALUE_NULL;
    result->response.api.result.start = 0;
    result->response.api.result.length = 0;
    result->response.api.result.bool_value = 0;
}

void tg_bot_update_summary_init(tg_bot_update_summary *update)
{
    if (update == 0) {
        return;
    }
    update->has_update = 0;
    update->has_message = 0;
    update->has_date = 0;
    update->has_sender_name = 0;
    update->has_text = 0;
    update->message_kind = TG_BOT_MESSAGE_NONE;
    update->update_id[0] = '\0';
    update->chat_id[0] = '\0';
    update->date[0] = '\0';
    update->sender_name[0] = '\0';
    update->text = 0;
    update->text_length = 0;
}

tg_bot_status tg_bot_parse_get_me_http_response(const char *http_response,
                                                unsigned long http_response_length,
                                                tg_bot_call_result *result)
{
    return tg_bot_parse_http_response(http_response, http_response_length, result);
}

tg_bot_status tg_bot_parse_get_updates_http_response(const char *http_response,
                                                     unsigned long http_response_length,
                                                     tg_bot_call_result *result)
{
    return tg_bot_parse_http_response(http_response, http_response_length, result);
}

tg_bot_status tg_bot_get_updates_first(const tg_bot_call_result *result,
                                       tg_bot_update_summary *update)
{
    return tg_bot_get_updates_at(result, 0, update);
}

tg_bot_status tg_bot_get_updates_at(const tg_bot_call_result *result,
                                    unsigned long index,
                                    tg_bot_update_summary *update)
{
    tg_json_status json_status;
    tg_json_value selected_update;
    tg_json_value message;
    tg_json_value chat;
    tg_json_value from;
    tg_json_value text;
    tg_json_value optional_value;
    unsigned long copied_length;

    if (update != 0) {
        tg_bot_update_summary_init(update);
    }
    if (result == 0 || update == 0) {
        return TG_BOT_INVALID_ARGUMENT;
    }
    if (!result->response.api.has_result ||
        result->response.api.result.type != TG_JSON_VALUE_ARRAY) {
        return TG_BOT_UPDATE_ERROR;
    }

    json_status = tg_json_array_get(result->response.api.result.start,
                                    result->response.api.result.length,
                                    index, &selected_update);
    if (json_status == TG_JSON_NOT_FOUND) {
        return TG_BOT_OK;
    }
    if (json_status != TG_JSON_OK ||
        selected_update.type != TG_JSON_VALUE_OBJECT) {
        return TG_BOT_UPDATE_ERROR;
    }

    update->has_update = 1;
    json_status = tg_json_object_get_number_copy(selected_update.start,
                                                 selected_update.length,
                                                 "update_id",
                                                 update->update_id,
                                                 sizeof(update->update_id),
                                                 &copied_length);
    if (json_status != TG_JSON_OK) {
        return TG_BOT_UPDATE_ERROR;
    }

    json_status = tg_json_object_get(selected_update.start, selected_update.length,
                                     "message", &message);
    if (json_status == TG_JSON_NOT_FOUND) {
        return TG_BOT_OK;
    }
    if (json_status != TG_JSON_OK || message.type != TG_JSON_VALUE_OBJECT) {
        return TG_BOT_UPDATE_ERROR;
    }
    update->has_message = 1;
    update->message_kind = TG_BOT_MESSAGE_UNSUPPORTED;

    json_status = tg_json_object_get_number_copy(message.start, message.length,
                                                 "date",
                                                 update->date,
                                                 sizeof(update->date),
                                                 &copied_length);
    if (json_status == TG_JSON_OK) {
        update->has_date = 1;
    }

    json_status = tg_json_object_get(message.start, message.length, "chat", &chat);
    if (json_status != TG_JSON_OK || chat.type != TG_JSON_VALUE_OBJECT) {
        return TG_BOT_UPDATE_ERROR;
    }
    json_status = tg_json_object_get_number_copy(chat.start, chat.length, "id",
                                                 update->chat_id,
                                                 sizeof(update->chat_id),
                                                 &copied_length);
    if (json_status != TG_JSON_OK) {
        return TG_BOT_UPDATE_ERROR;
    }

    json_status = tg_json_object_get(message.start, message.length, "from", &from);
    if (json_status == TG_JSON_OK && from.type == TG_JSON_VALUE_OBJECT) {
        json_status = tg_json_object_get_string_copy(from.start, from.length,
                                                     "username",
                                                     update->sender_name,
                                                     sizeof(update->sender_name),
                                                     &copied_length);
        if (json_status == TG_JSON_OK) {
            update->has_sender_name = 1;
        } else if (json_status == TG_JSON_NOT_FOUND) {
            json_status = tg_json_object_get_string_copy(from.start, from.length,
                                                         "first_name",
                                                         update->sender_name,
                                                         sizeof(update->sender_name),
                                                         &copied_length);
            if (json_status == TG_JSON_OK) {
                update->has_sender_name = 1;
            }
        }
    }

    json_status = tg_json_object_get(message.start, message.length, "text", &text);
    if (json_status == TG_JSON_NOT_FOUND) {
        json_status = tg_json_object_get(message.start, message.length,
                                         "photo", &optional_value);
        if (json_status == TG_JSON_OK && optional_value.type == TG_JSON_VALUE_ARRAY) {
            update->message_kind = TG_BOT_MESSAGE_PHOTO;
            return TG_BOT_OK;
        }
        json_status = tg_json_object_get(message.start, message.length,
                                         "sticker", &optional_value);
        if (json_status == TG_JSON_OK && optional_value.type == TG_JSON_VALUE_OBJECT) {
            update->message_kind = TG_BOT_MESSAGE_STICKER;
            return TG_BOT_OK;
        }
        json_status = tg_json_object_get(message.start, message.length,
                                         "document", &optional_value);
        if (json_status == TG_JSON_OK && optional_value.type == TG_JSON_VALUE_OBJECT) {
            update->message_kind = TG_BOT_MESSAGE_DOCUMENT;
            return TG_BOT_OK;
        }
        return TG_BOT_OK;
    }
    if (json_status != TG_JSON_OK || text.type != TG_JSON_VALUE_STRING) {
        return TG_BOT_UPDATE_ERROR;
    }

    update->has_text = 1;
    update->message_kind = tg_bot_raw_json_string_starts_with(&text, '/') ?
                           TG_BOT_MESSAGE_COMMAND : TG_BOT_MESSAGE_TEXT;
    update->text = text.start;
    update->text_length = text.length;
    return TG_BOT_OK;
}

const char *tg_bot_message_kind_name(tg_bot_message_kind kind)
{
    switch (kind) {
    case TG_BOT_MESSAGE_NONE:
        return "none";
    case TG_BOT_MESSAGE_TEXT:
        return "text";
    case TG_BOT_MESSAGE_COMMAND:
        return "command";
    case TG_BOT_MESSAGE_PHOTO:
        return "photo";
    case TG_BOT_MESSAGE_STICKER:
        return "sticker";
    case TG_BOT_MESSAGE_DOCUMENT:
        return "document";
    case TG_BOT_MESSAGE_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}

tg_bot_status tg_bot_update_next_offset(const tg_bot_update_summary *update,
                                        char *next_offset,
                                        unsigned long next_offset_size)
{
    char reversed[32];
    unsigned long input_pos;
    unsigned long reversed_length;
    unsigned long output_pos;
    int carry;

    if (next_offset != 0 && next_offset_size > 0) {
        next_offset[0] = '\0';
    }
    if (update == 0 || next_offset == 0 || next_offset_size == 0 ||
        !update->has_update || !tg_bot_is_decimal_text(update->update_id)) {
        return TG_BOT_INVALID_ARGUMENT;
    }

    input_pos = (unsigned long)strlen(update->update_id);
    reversed_length = 0;
    carry = 1;

    while (input_pos > 0) {
        int digit;

        --input_pos;
        digit = (update->update_id[input_pos] - '0') + carry;
        if (digit >= 10) {
            digit -= 10;
            carry = 1;
        } else {
            carry = 0;
        }
        if (reversed_length + 1 >= sizeof(reversed)) {
            return TG_BOT_BODY_ERROR;
        }
        reversed[reversed_length] = (char)('0' + digit);
        ++reversed_length;
    }

    if (carry) {
        if (reversed_length + 1 >= sizeof(reversed)) {
            return TG_BOT_BODY_ERROR;
        }
        reversed[reversed_length] = '1';
        ++reversed_length;
    }

    if (reversed_length + 1 > next_offset_size) {
        return TG_BOT_BODY_ERROR;
    }

    output_pos = 0;
    while (reversed_length > 0) {
        --reversed_length;
        next_offset[output_pos] = reversed[reversed_length];
        ++output_pos;
    }
    next_offset[output_pos] = '\0';
    return TG_BOT_OK;
}

tg_bot_status tg_bot_parse_send_message_http_response(const char *http_response,
                                                      unsigned long http_response_length,
                                                      tg_bot_call_result *result)
{
    return tg_bot_parse_http_response(http_response, http_response_length, result);
}

tg_bot_status tg_bot_build_send_message_body(const char *chat_id, const char *text,
                                             char *body_buffer,
                                             unsigned long body_buffer_size,
                                             unsigned long *body_length)
{
    unsigned long position;

    if (body_length != 0) {
        *body_length = 0;
    }
    if (body_buffer != 0 && body_buffer_size > 0) {
        body_buffer[0] = '\0';
    }
    if (chat_id == 0 || text == 0 || body_buffer == 0 || body_buffer_size == 0 ||
        body_length == 0 || chat_id[0] == '\0') {
        return TG_BOT_INVALID_ARGUMENT;
    }

    position = 0;
    if (tg_bot_append_text(body_buffer, body_buffer_size, &position,
                           "{\"chat_id\":") != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }
    if (tg_bot_append_json_string(body_buffer, body_buffer_size, &position,
                                  chat_id) != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }
    if (tg_bot_append_text(body_buffer, body_buffer_size, &position,
                           ",\"text\":") != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }
    if (tg_bot_append_json_string(body_buffer, body_buffer_size, &position,
                                  text) != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }
    if (tg_bot_append_char(body_buffer, body_buffer_size, &position, '}') != TG_BOT_OK) {
        return TG_BOT_BODY_ERROR;
    }

    *body_length = position;
    return TG_BOT_OK;
}

tg_bot_status tg_bot_get_me_from_token_file(const char *token_file_path,
                                            char *http_buffer,
                                            unsigned long http_buffer_size,
                                            unsigned long *http_response_length,
                                            tg_bot_call_result *result,
                                            char *error_buffer,
                                            unsigned long error_buffer_size)
{
    tg_telegram_status telegram_status;
    tg_https_status https_status;
    char token[128];
    char path[256];
    unsigned long token_length;
    unsigned long path_length;

    if (http_response_length != 0) {
        *http_response_length = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    if (token_file_path == 0 || http_buffer == 0 || http_buffer_size == 0 ||
        http_response_length == 0 || result == 0) {
        return TG_BOT_INVALID_ARGUMENT;
    }

    tg_bot_call_result_init(result);

    telegram_status = tg_telegram_load_token_file(token_file_path, token, sizeof(token),
                                                  &token_length, &result->file_status);
    if (telegram_status != TG_TELEGRAM_OK) {
        result->telegram_status = telegram_status;
        return TG_BOT_TOKEN_ERROR;
    }

    telegram_status = tg_telegram_build_bot_path(token, "getMe", path, sizeof(path),
                                                 &path_length);
    if (telegram_status != TG_TELEGRAM_OK) {
        result->telegram_status = telegram_status;
        return TG_BOT_PATH_ERROR;
    }

    https_status = tg_https_get(tg_telegram_api_host(), "443", path,
                                http_buffer, http_buffer_size,
                                http_response_length, &result->tls_status,
                                &result->net_status, error_buffer,
                                error_buffer_size);
    result->https_status = https_status;
    if (https_status != TG_HTTPS_OK) {
        return TG_BOT_HTTPS_ERROR;
    }

    return tg_bot_parse_get_me_http_response(http_buffer, *http_response_length, result);
}

tg_bot_status tg_bot_get_updates_from_token_file(const char *token_file_path,
                                                 char *http_buffer,
                                                 unsigned long http_buffer_size,
                                                 unsigned long *http_response_length,
                                                 tg_bot_call_result *result,
                                                 char *error_buffer,
                                                 unsigned long error_buffer_size)
{
    return tg_bot_get_updates_from_token_file_with_offset(
        token_file_path, 0, http_buffer, http_buffer_size, http_response_length,
        result, error_buffer, error_buffer_size);
}

tg_bot_status tg_bot_get_updates_from_token_file_with_offset(
    const char *token_file_path,
    const char *offset,
    char *http_buffer,
    unsigned long http_buffer_size,
    unsigned long *http_response_length,
    tg_bot_call_result *result,
    char *error_buffer,
    unsigned long error_buffer_size)
{
    tg_telegram_status telegram_status;
    tg_https_status https_status;
    char token[128];
    char path[256];
    char method[64];
    unsigned long token_length;
    unsigned long path_length;

    if (http_response_length != 0) {
        *http_response_length = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    if (token_file_path == 0 || http_buffer == 0 || http_buffer_size == 0 ||
        http_response_length == 0 || result == 0) {
        return TG_BOT_INVALID_ARGUMENT;
    }
    if (offset != 0 && offset[0] != '\0' && !tg_bot_is_decimal_text(offset)) {
        return TG_BOT_INVALID_ARGUMENT;
    }

    tg_bot_call_result_init(result);

    telegram_status = tg_telegram_load_token_file(token_file_path, token, sizeof(token),
                                                  &token_length, &result->file_status);
    if (telegram_status != TG_TELEGRAM_OK) {
        result->telegram_status = telegram_status;
        return TG_BOT_TOKEN_ERROR;
    }

    method[0] = '\0';
    path_length = 0;
    if (tg_bot_append_text(method, sizeof(method), &path_length,
                           "getUpdates") != TG_BOT_OK) {
        return TG_BOT_PATH_ERROR;
    }
    if (offset != 0 && offset[0] != '\0') {
        if (tg_bot_append_text(method, sizeof(method), &path_length,
                               "?offset=") != TG_BOT_OK ||
            tg_bot_append_text(method, sizeof(method), &path_length,
                               offset) != TG_BOT_OK) {
            return TG_BOT_PATH_ERROR;
        }
    }

    telegram_status = tg_telegram_build_bot_path(token, method, path, sizeof(path),
                                                 &path_length);
    if (telegram_status != TG_TELEGRAM_OK) {
        result->telegram_status = telegram_status;
        return TG_BOT_PATH_ERROR;
    }

    https_status = tg_https_get(tg_telegram_api_host(), "443", path,
                                http_buffer, http_buffer_size,
                                http_response_length, &result->tls_status,
                                &result->net_status, error_buffer,
                                error_buffer_size);
    result->https_status = https_status;
    if (https_status != TG_HTTPS_OK) {
        return TG_BOT_HTTPS_ERROR;
    }

    return tg_bot_parse_get_updates_http_response(http_buffer, *http_response_length,
                                                 result);
}

tg_bot_status tg_bot_send_message_from_token_file(const char *token_file_path,
                                                  const char *chat_id,
                                                  const char *text,
                                                  char *http_buffer,
                                                  unsigned long http_buffer_size,
                                                  unsigned long *http_response_length,
                                                  tg_bot_call_result *result,
                                                  char *error_buffer,
                                                  unsigned long error_buffer_size)
{
    tg_telegram_status telegram_status;
    tg_https_status https_status;
    tg_bot_status body_status;
    char token[128];
    char path[256];
    char body[2048];
    unsigned long token_length;
    unsigned long path_length;
    unsigned long body_length;

    if (http_response_length != 0) {
        *http_response_length = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    if (token_file_path == 0 || chat_id == 0 || text == 0 || http_buffer == 0 ||
        http_buffer_size == 0 || http_response_length == 0 || result == 0) {
        return TG_BOT_INVALID_ARGUMENT;
    }

    tg_bot_call_result_init(result);

    telegram_status = tg_telegram_load_token_file(token_file_path, token, sizeof(token),
                                                  &token_length, &result->file_status);
    if (telegram_status != TG_TELEGRAM_OK) {
        result->telegram_status = telegram_status;
        return TG_BOT_TOKEN_ERROR;
    }

    telegram_status = tg_telegram_build_bot_path(token, "sendMessage", path, sizeof(path),
                                                 &path_length);
    if (telegram_status != TG_TELEGRAM_OK) {
        result->telegram_status = telegram_status;
        return TG_BOT_PATH_ERROR;
    }

    body_status = tg_bot_build_send_message_body(chat_id, text, body, sizeof(body),
                                                 &body_length);
    if (body_status != TG_BOT_OK) {
        return body_status;
    }

    https_status = tg_https_post(tg_telegram_api_host(), "443", path,
                                 "application/json", body, body_length,
                                 http_buffer, http_buffer_size,
                                 http_response_length, &result->tls_status,
                                 &result->net_status, error_buffer,
                                 error_buffer_size);
    result->https_status = https_status;
    if (https_status != TG_HTTPS_OK) {
        return TG_BOT_HTTPS_ERROR;
    }

    return tg_bot_parse_send_message_http_response(http_buffer, *http_response_length,
                                                  result);
}

const char *tg_bot_status_name(tg_bot_status status)
{
    switch (status) {
    case TG_BOT_OK:
        return "ok";
    case TG_BOT_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_BOT_TOKEN_ERROR:
        return "token-error";
    case TG_BOT_PATH_ERROR:
        return "path-error";
    case TG_BOT_HTTPS_ERROR:
        return "https-error";
    case TG_BOT_TELEGRAM_ERROR:
        return "telegram-error";
    case TG_BOT_BODY_ERROR:
        return "body-error";
    case TG_BOT_UPDATE_ERROR:
        return "update-error";
    default:
        return "unknown";
    }
}
