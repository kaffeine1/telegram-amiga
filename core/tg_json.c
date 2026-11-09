/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_json.h"

static int tg_json_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int tg_json_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int tg_json_is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int tg_json_hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static void tg_json_skip_ws(const char *json, unsigned long json_length, unsigned long *pos)
{
    while (*pos < json_length && tg_json_is_space(json[*pos])) {
        *pos = *pos + 1;
    }
}

/* JSON strings may contain escaped quotes; this scans to the real closing quote. */
static tg_json_status tg_json_scan_string(const char *json, unsigned long json_length,
                                          unsigned long pos, unsigned long *content_start,
                                          unsigned long *content_length,
                                          unsigned long *next_pos)
{
    unsigned long i;

    if (pos >= json_length || json[pos] != '"') {
        return TG_JSON_INVALID_JSON;
    }

    i = pos + 1;
    while (i < json_length) {
        if (json[i] == '\\') {
            unsigned long j;

            if (i + 1 >= json_length) {
                return TG_JSON_INVALID_JSON;
            }
            if (json[i + 1] == '"' || json[i + 1] == '\\' ||
                json[i + 1] == '/' || json[i + 1] == 'b' ||
                json[i + 1] == 'f' || json[i + 1] == 'n' ||
                json[i + 1] == 'r' || json[i + 1] == 't') {
                i += 2;
            } else if (json[i + 1] == 'u') {
                if (i + 5 >= json_length) {
                    return TG_JSON_INVALID_JSON;
                }
                for (j = i + 2; j < i + 6; ++j) {
                    if (!tg_json_is_hex_digit(json[j])) {
                        return TG_JSON_INVALID_JSON;
                    }
                }
                i += 6;
            } else {
                return TG_JSON_INVALID_JSON;
            }
        } else if (json[i] == '"') {
            *content_start = pos + 1;
            *content_length = i - (pos + 1);
            *next_pos = i + 1;
            return TG_JSON_OK;
        } else if ((unsigned char)json[i] < 0x20) {
            return TG_JSON_INVALID_JSON;
        } else {
            ++i;
        }
    }

    return TG_JSON_INVALID_JSON;
}

static int tg_json_key_equals(const char *json, unsigned long key_start,
                              unsigned long key_length, const char *field_name)
{
    /* Telegram field names are plain ASCII, so escaped object keys are not needed yet. */
    return strlen(field_name) == key_length &&
           strncmp(json + key_start, field_name, key_length) == 0;
}

static tg_json_status tg_json_scan_literal(const char *json, unsigned long json_length,
                                           unsigned long pos, const char *literal,
                                           unsigned long *next_pos)
{
    unsigned long literal_length;

    literal_length = (unsigned long)strlen(literal);
    if (pos + literal_length > json_length) {
        return TG_JSON_INVALID_JSON;
    }
    if (strncmp(json + pos, literal, literal_length) != 0) {
        return TG_JSON_INVALID_JSON;
    }

    *next_pos = pos + literal_length;
    return TG_JSON_OK;
}

static tg_json_status tg_json_scan_number(const char *json, unsigned long json_length,
                                          unsigned long pos, unsigned long *next_pos)
{
    unsigned long i;

    i = pos;
    if (i < json_length && json[i] == '-') {
        ++i;
    }
    if (i >= json_length || !tg_json_is_digit(json[i])) {
        return TG_JSON_INVALID_JSON;
    }
    if (json[i] == '0') {
        ++i;
    } else {
        while (i < json_length && tg_json_is_digit(json[i])) {
            ++i;
        }
    }
    if (i < json_length && json[i] == '.') {
        ++i;
        if (i >= json_length || !tg_json_is_digit(json[i])) {
            return TG_JSON_INVALID_JSON;
        }
        while (i < json_length && tg_json_is_digit(json[i])) {
            ++i;
        }
    }
    if (i < json_length && (json[i] == 'e' || json[i] == 'E')) {
        ++i;
        if (i < json_length && (json[i] == '+' || json[i] == '-')) {
            ++i;
        }
        if (i >= json_length || !tg_json_is_digit(json[i])) {
            return TG_JSON_INVALID_JSON;
        }
        while (i < json_length && tg_json_is_digit(json[i])) {
            ++i;
        }
    }

    *next_pos = i;
    return TG_JSON_OK;
}

/* Objects and arrays can be nested; this keeps enough state to skip them safely. */
static tg_json_status tg_json_scan_container(const char *json, unsigned long json_length,
                                             unsigned long pos, char open_char,
                                             char close_char, unsigned long *next_pos)
{
    unsigned long i;
    unsigned long depth;

    if (pos >= json_length || json[pos] != open_char) {
        return TG_JSON_INVALID_JSON;
    }

    i = pos + 1;
    depth = 1;
    while (i < json_length) {
        if (json[i] == '"') {
            unsigned long string_start;
            unsigned long string_length;
            tg_json_status status;

            status = tg_json_scan_string(json, json_length, i, &string_start,
                                         &string_length, &i);
            if (status != TG_JSON_OK) {
                return status;
            }
        } else if (json[i] == open_char) {
            ++depth;
            ++i;
        } else if (json[i] == close_char) {
            --depth;
            ++i;
            if (depth == 0) {
                *next_pos = i;
                return TG_JSON_OK;
            }
        } else {
            ++i;
        }
    }

    return TG_JSON_INVALID_JSON;
}

static tg_json_status tg_json_scan_value(const char *json, unsigned long json_length,
                                         unsigned long pos, tg_json_value *value,
                                         unsigned long *next_pos)
{
    unsigned long value_start;
    unsigned long value_length;
    tg_json_status status;

    tg_json_skip_ws(json, json_length, &pos);
    if (pos >= json_length) {
        return TG_JSON_INVALID_JSON;
    }

    value->start = json + pos;
    value->length = 0;
    value->bool_value = 0;

    if (json[pos] == '"') {
        status = tg_json_scan_string(json, json_length, pos, &value_start,
                                     &value_length, next_pos);
        if (status != TG_JSON_OK) {
            return status;
        }
        value->type = TG_JSON_VALUE_STRING;
        value->start = json + value_start;
        value->length = value_length;
        return TG_JSON_OK;
    }

    if (json[pos] == '{') {
        status = tg_json_scan_container(json, json_length, pos, '{', '}', next_pos);
        if (status != TG_JSON_OK) {
            return status;
        }
        value->type = TG_JSON_VALUE_OBJECT;
        value->length = *next_pos - pos;
        return TG_JSON_OK;
    }

    if (json[pos] == '[') {
        status = tg_json_scan_container(json, json_length, pos, '[', ']', next_pos);
        if (status != TG_JSON_OK) {
            return status;
        }
        value->type = TG_JSON_VALUE_ARRAY;
        value->length = *next_pos - pos;
        return TG_JSON_OK;
    }

    if (json[pos] == 't') {
        status = tg_json_scan_literal(json, json_length, pos, "true", next_pos);
        value->type = TG_JSON_VALUE_BOOL;
        value->length = 4;
        value->bool_value = 1;
        return status;
    }

    if (json[pos] == 'f') {
        status = tg_json_scan_literal(json, json_length, pos, "false", next_pos);
        value->type = TG_JSON_VALUE_BOOL;
        value->length = 5;
        value->bool_value = 0;
        return status;
    }

    if (json[pos] == 'n') {
        status = tg_json_scan_literal(json, json_length, pos, "null", next_pos);
        value->type = TG_JSON_VALUE_NULL;
        value->length = 4;
        return status;
    }

    status = tg_json_scan_number(json, json_length, pos, next_pos);
    if (status == TG_JSON_OK) {
        value->type = TG_JSON_VALUE_NUMBER;
        value->length = *next_pos - pos;
    }
    return status;
}

tg_json_status tg_json_object_get(const char *json, unsigned long json_length,
                                  const char *field_name, tg_json_value *value)
{
    unsigned long pos;

    if (value != 0) {
        value->type = TG_JSON_VALUE_NULL;
        value->start = 0;
        value->length = 0;
        value->bool_value = 0;
    }
    if (json == 0 || field_name == 0 || value == 0) {
        return TG_JSON_INVALID_ARGUMENT;
    }

    pos = 0;
    tg_json_skip_ws(json, json_length, &pos);
    if (pos >= json_length || json[pos] != '{') {
        return TG_JSON_INVALID_JSON;
    }
    ++pos;

    for (;;) {
        unsigned long key_start;
        unsigned long key_length;
        unsigned long next_pos;
        tg_json_status status;

        tg_json_skip_ws(json, json_length, &pos);
        if (pos < json_length && json[pos] == '}') {
            return TG_JSON_NOT_FOUND;
        }

        status = tg_json_scan_string(json, json_length, pos, &key_start,
                                     &key_length, &pos);
        if (status != TG_JSON_OK) {
            return status;
        }

        tg_json_skip_ws(json, json_length, &pos);
        if (pos >= json_length || json[pos] != ':') {
            return TG_JSON_INVALID_JSON;
        }
        ++pos;

        status = tg_json_scan_value(json, json_length, pos, value, &next_pos);
        if (status != TG_JSON_OK) {
            return status;
        }
        if (tg_json_key_equals(json, key_start, key_length, field_name)) {
            return TG_JSON_OK;
        }

        pos = next_pos;
        tg_json_skip_ws(json, json_length, &pos);
        if (pos < json_length && json[pos] == ',') {
            ++pos;
        } else if (pos < json_length && json[pos] == '}') {
            return TG_JSON_NOT_FOUND;
        } else {
            return TG_JSON_INVALID_JSON;
        }
    }
}

tg_json_status tg_json_array_first(const char *array_json, unsigned long array_json_length,
                                   tg_json_value *value)
{
    return tg_json_array_get(array_json, array_json_length, 0, value);
}

tg_json_status tg_json_array_get(const char *array_json, unsigned long array_json_length,
                                 unsigned long index, tg_json_value *value)
{
    unsigned long pos;
    unsigned long next_pos;
    unsigned long current_index;

    if (value != 0) {
        value->type = TG_JSON_VALUE_NULL;
        value->start = 0;
        value->length = 0;
        value->bool_value = 0;
    }
    if (array_json == 0 || value == 0) {
        return TG_JSON_INVALID_ARGUMENT;
    }

    pos = 0;
    tg_json_skip_ws(array_json, array_json_length, &pos);
    if (pos >= array_json_length || array_json[pos] != '[') {
        return TG_JSON_INVALID_JSON;
    }
    ++pos;

    tg_json_skip_ws(array_json, array_json_length, &pos);
    if (pos < array_json_length && array_json[pos] == ']') {
        return TG_JSON_NOT_FOUND;
    }

    current_index = 0;
    for (;;) {
        tg_json_status status;

        status = tg_json_scan_value(array_json, array_json_length, pos, value,
                                    &next_pos);
        if (status != TG_JSON_OK) {
            return status;
        }
        if (current_index == index) {
            return TG_JSON_OK;
        }

        pos = next_pos;
        tg_json_skip_ws(array_json, array_json_length, &pos);
        if (pos < array_json_length && array_json[pos] == ',') {
            ++pos;
            ++current_index;
        } else if (pos < array_json_length && array_json[pos] == ']') {
            return TG_JSON_NOT_FOUND;
        } else {
            return TG_JSON_INVALID_JSON;
        }
    }
}

tg_json_status tg_json_object_get_bool(const char *json, unsigned long json_length,
                                       const char *field_name, int *bool_value)
{
    tg_json_value value;
    tg_json_status status;

    if (bool_value == 0) {
        return TG_JSON_INVALID_ARGUMENT;
    }

    status = tg_json_object_get(json, json_length, field_name, &value);
    if (status != TG_JSON_OK) {
        return status;
    }
    if (value.type != TG_JSON_VALUE_BOOL) {
        return TG_JSON_TYPE_MISMATCH;
    }

    *bool_value = value.bool_value;
    return TG_JSON_OK;
}

static tg_json_status tg_json_decode_append_byte(char *buffer,
                                                 unsigned long buffer_size,
                                                 unsigned long *position,
                                                 unsigned char byte)
{
    if (*position + 1 >= buffer_size) {
        return TG_JSON_BUFFER_TOO_SMALL;
    }

    buffer[*position] = (char)byte;
    ++(*position);
    buffer[*position] = '\0';
    return TG_JSON_OK;
}

static tg_json_status tg_json_decode_append_utf8(char *buffer,
                                                 unsigned long buffer_size,
                                                 unsigned long *position,
                                                 unsigned long code_point)
{
    if (code_point <= 0x7fUL) {
        return tg_json_decode_append_byte(buffer, buffer_size, position,
                                          (unsigned char)code_point);
    }
    if (code_point <= 0x7ffUL) {
        if (tg_json_decode_append_byte(buffer, buffer_size, position,
                                       (unsigned char)(0xc0UL | (code_point >> 6))) != TG_JSON_OK) {
            return TG_JSON_BUFFER_TOO_SMALL;
        }
        return tg_json_decode_append_byte(buffer, buffer_size, position,
                                          (unsigned char)(0x80UL | (code_point & 0x3fUL)));
    }
    if (code_point <= 0xffffUL) {
        if (tg_json_decode_append_byte(buffer, buffer_size, position,
                                       (unsigned char)(0xe0UL | (code_point >> 12))) != TG_JSON_OK ||
            tg_json_decode_append_byte(buffer, buffer_size, position,
                                       (unsigned char)(0x80UL | ((code_point >> 6) & 0x3fUL))) != TG_JSON_OK) {
            return TG_JSON_BUFFER_TOO_SMALL;
        }
        return tg_json_decode_append_byte(buffer, buffer_size, position,
                                          (unsigned char)(0x80UL | (code_point & 0x3fUL)));
    }
    if (code_point <= 0x10ffffUL) {
        if (tg_json_decode_append_byte(buffer, buffer_size, position,
                                       (unsigned char)(0xf0UL | (code_point >> 18))) != TG_JSON_OK ||
            tg_json_decode_append_byte(buffer, buffer_size, position,
                                       (unsigned char)(0x80UL | ((code_point >> 12) & 0x3fUL))) != TG_JSON_OK ||
            tg_json_decode_append_byte(buffer, buffer_size, position,
                                       (unsigned char)(0x80UL | ((code_point >> 6) & 0x3fUL))) != TG_JSON_OK) {
            return TG_JSON_BUFFER_TOO_SMALL;
        }
        return tg_json_decode_append_byte(buffer, buffer_size, position,
                                          (unsigned char)(0x80UL | (code_point & 0x3fUL)));
    }

    return TG_JSON_INVALID_JSON;
}

static tg_json_status tg_json_decode_hex4(const char *text,
                                          unsigned long text_length,
                                          unsigned long position,
                                          unsigned long *code_unit)
{
    unsigned long value;
    unsigned long i;
    int digit;

    if (code_unit == 0 || position + 4 > text_length) {
        return TG_JSON_INVALID_JSON;
    }

    value = 0;
    for (i = 0; i < 4; ++i) {
        digit = tg_json_hex_value(text[position + i]);
        if (digit < 0) {
            return TG_JSON_INVALID_JSON;
        }
        value = (value << 4) | (unsigned long)digit;
    }

    *code_unit = value;
    return TG_JSON_OK;
}

tg_json_status tg_json_string_decode(const char *raw_string,
                                     unsigned long raw_string_length,
                                     char *buffer,
                                     unsigned long buffer_size,
                                     unsigned long *string_length)
{
    unsigned long input_pos;
    unsigned long output_pos;
    tg_json_status status;

    if (string_length != 0) {
        *string_length = 0;
    }
    if (buffer != 0 && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (raw_string == 0 || buffer == 0 || buffer_size == 0 ||
        string_length == 0) {
        return TG_JSON_INVALID_ARGUMENT;
    }

    input_pos = 0;
    output_pos = 0;
    while (input_pos < raw_string_length) {
        unsigned char c;

        c = (unsigned char)raw_string[input_pos];
        if (c == '\\') {
            unsigned char escape;

            if (input_pos + 1 >= raw_string_length) {
                return TG_JSON_INVALID_JSON;
            }
            escape = (unsigned char)raw_string[input_pos + 1];
            if (escape == '"' || escape == '\\' || escape == '/') {
                status = tg_json_decode_append_byte(buffer, buffer_size,
                                                    &output_pos, escape);
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 2;
            } else if (escape == 'b') {
                status = tg_json_decode_append_byte(buffer, buffer_size,
                                                    &output_pos, '\b');
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 2;
            } else if (escape == 'f') {
                status = tg_json_decode_append_byte(buffer, buffer_size,
                                                    &output_pos, '\f');
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 2;
            } else if (escape == 'n') {
                status = tg_json_decode_append_byte(buffer, buffer_size,
                                                    &output_pos, '\n');
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 2;
            } else if (escape == 'r') {
                status = tg_json_decode_append_byte(buffer, buffer_size,
                                                    &output_pos, '\r');
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 2;
            } else if (escape == 't') {
                status = tg_json_decode_append_byte(buffer, buffer_size,
                                                    &output_pos, '\t');
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 2;
            } else if (escape == 'u') {
                unsigned long code_unit;
                unsigned long code_point;

                status = tg_json_decode_hex4(raw_string, raw_string_length,
                                             input_pos + 2, &code_unit);
                if (status != TG_JSON_OK) {
                    return status;
                }
                input_pos += 6;
                if (code_unit >= 0xd800UL && code_unit <= 0xdbffUL) {
                    unsigned long low_unit;

                    if (input_pos + 6 > raw_string_length ||
                        raw_string[input_pos] != '\\' ||
                        raw_string[input_pos + 1] != 'u') {
                        return TG_JSON_INVALID_JSON;
                    }
                    status = tg_json_decode_hex4(raw_string, raw_string_length,
                                                 input_pos + 2, &low_unit);
                    if (status != TG_JSON_OK ||
                        low_unit < 0xdc00UL || low_unit > 0xdfffUL) {
                        return TG_JSON_INVALID_JSON;
                    }
                    code_point = 0x10000UL +
                                 ((code_unit - 0xd800UL) << 10) +
                                 (low_unit - 0xdc00UL);
                    input_pos += 6;
                } else if (code_unit >= 0xdc00UL && code_unit <= 0xdfffUL) {
                    return TG_JSON_INVALID_JSON;
                } else {
                    code_point = code_unit;
                }

                status = tg_json_decode_append_utf8(buffer, buffer_size,
                                                    &output_pos, code_point);
                if (status != TG_JSON_OK) {
                    return status;
                }
            } else {
                return TG_JSON_INVALID_JSON;
            }
        } else {
            if (c < 0x20) {
                return TG_JSON_INVALID_JSON;
            }
            status = tg_json_decode_append_byte(buffer, buffer_size,
                                                &output_pos, c);
            if (status != TG_JSON_OK) {
                return status;
            }
            ++input_pos;
        }
    }

    buffer[output_pos] = '\0';
    *string_length = output_pos;
    return TG_JSON_OK;
}

tg_json_status tg_json_object_get_string_copy(const char *json, unsigned long json_length,
                                              const char *field_name, char *buffer,
                                              unsigned long buffer_size,
                                              unsigned long *string_length)
{
    tg_json_value value;
    tg_json_status status;

    if (string_length != 0) {
        *string_length = 0;
    }
    if (buffer == 0 || buffer_size == 0 || string_length == 0) {
        return TG_JSON_INVALID_ARGUMENT;
    }

    status = tg_json_object_get(json, json_length, field_name, &value);
    if (status != TG_JSON_OK) {
        return status;
    }
    if (value.type != TG_JSON_VALUE_STRING) {
        return TG_JSON_TYPE_MISMATCH;
    }
    return tg_json_string_decode(value.start, value.length, buffer,
                                 buffer_size, string_length);
}

tg_json_status tg_json_object_get_number_copy(const char *json, unsigned long json_length,
                                              const char *field_name, char *buffer,
                                              unsigned long buffer_size,
                                              unsigned long *number_length)
{
    tg_json_value value;
    tg_json_status status;

    if (number_length != 0) {
        *number_length = 0;
    }
    if (buffer == 0 || buffer_size == 0 || number_length == 0) {
        return TG_JSON_INVALID_ARGUMENT;
    }

    status = tg_json_object_get(json, json_length, field_name, &value);
    if (status != TG_JSON_OK) {
        return status;
    }
    if (value.type != TG_JSON_VALUE_NUMBER) {
        return TG_JSON_TYPE_MISMATCH;
    }
    if (value.length + 1 > buffer_size) {
        return TG_JSON_BUFFER_TOO_SMALL;
    }

    memcpy(buffer, value.start, value.length);
    buffer[value.length] = '\0';
    *number_length = value.length;
    return TG_JSON_OK;
}

const char *tg_json_status_name(tg_json_status status)
{
    switch (status) {
    case TG_JSON_OK:
        return "ok";
    case TG_JSON_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_JSON_INVALID_JSON:
        return "invalid-json";
    case TG_JSON_NOT_FOUND:
        return "not-found";
    case TG_JSON_TYPE_MISMATCH:
        return "type-mismatch";
    case TG_JSON_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    default:
        return "unknown";
    }
}

const char *tg_json_value_type_name(tg_json_value_type type)
{
    switch (type) {
    case TG_JSON_VALUE_NULL:
        return "null";
    case TG_JSON_VALUE_BOOL:
        return "bool";
    case TG_JSON_VALUE_NUMBER:
        return "number";
    case TG_JSON_VALUE_STRING:
        return "string";
    case TG_JSON_VALUE_OBJECT:
        return "object";
    case TG_JSON_VALUE_ARRAY:
        return "array";
    default:
        return "unknown";
    }
}
