/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_mtproto_tl.h"

void tg_mtproto_tl_writer_init(tg_mtproto_tl_writer *writer,
                               unsigned char *buffer,
                               unsigned long capacity)
{
    if (writer != 0) {
        writer->buffer = buffer;
        writer->capacity = capacity;
        writer->length = 0;
    }
}

void tg_mtproto_tl_reader_init(tg_mtproto_tl_reader *reader,
                               const unsigned char *buffer,
                               unsigned long length)
{
    if (reader != 0) {
        reader->buffer = buffer;
        reader->length = length;
        reader->offset = 0;
    }
}

static tg_mtproto_tl_status tg_mtproto_tl_reserve(tg_mtproto_tl_writer *writer,
                                                  unsigned long count)
{
    if (writer == 0 || writer->buffer == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (count > writer->capacity || writer->length > writer->capacity - count) {
        return TG_MTPROTO_TL_BUFFER_TOO_SMALL;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_write_u32(tg_mtproto_tl_writer *writer,
                                             unsigned long value)
{
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_reserve(writer, 4);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }

    writer->buffer[writer->length++] = (unsigned char)(value & 0xffUL);
    writer->buffer[writer->length++] = (unsigned char)((value >> 8) & 0xffUL);
    writer->buffer[writer->length++] = (unsigned char)((value >> 16) & 0xffUL);
    writer->buffer[writer->length++] = (unsigned char)((value >> 24) & 0xffUL);
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_write_u8(tg_mtproto_tl_writer *writer,
                                            unsigned long value)
{
    tg_mtproto_tl_status status;

    if (value > 0xffUL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    status = tg_mtproto_tl_reserve(writer, 1);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    writer->buffer[writer->length++] = (unsigned char)value;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_write_u64(tg_mtproto_tl_writer *writer,
                                             unsigned long hi,
                                             unsigned long lo)
{
    tg_mtproto_tl_status status;

    status = tg_mtproto_tl_write_u32(writer, lo);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return tg_mtproto_tl_write_u32(writer, hi);
}

tg_mtproto_tl_status tg_mtproto_tl_write_raw(tg_mtproto_tl_writer *writer,
                                             const unsigned char *data,
                                             unsigned long data_length)
{
    tg_mtproto_tl_status status;

    if (data == 0 && data_length > 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    status = tg_mtproto_tl_reserve(writer, data_length);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    if (data_length > 0) {
        memcpy(writer->buffer + writer->length, data, (size_t)data_length);
        writer->length += data_length;
    }
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_write_bytes(tg_mtproto_tl_writer *writer,
                                               const unsigned char *data,
                                               unsigned long data_length)
{
    unsigned long prefix_length;
    unsigned long total_without_padding;
    unsigned long padding;
    tg_mtproto_tl_status status;

    if (data == 0 && data_length > 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (data_length >= 0x1000000UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    prefix_length = data_length < 254UL ? 1UL : 4UL;
    total_without_padding = prefix_length + data_length;
    padding = (4UL - (total_without_padding % 4UL)) % 4UL;
    status = tg_mtproto_tl_reserve(writer, total_without_padding + padding);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }

    if (data_length < 254UL) {
        writer->buffer[writer->length++] = (unsigned char)data_length;
    } else {
        writer->buffer[writer->length++] = 254U;
        writer->buffer[writer->length++] = (unsigned char)(data_length & 0xffUL);
        writer->buffer[writer->length++] =
            (unsigned char)((data_length >> 8) & 0xffUL);
        writer->buffer[writer->length++] =
            (unsigned char)((data_length >> 16) & 0xffUL);
    }
    if (data_length > 0) {
        memcpy(writer->buffer + writer->length, data, (size_t)data_length);
        writer->length += data_length;
    }
    while (padding > 0) {
        writer->buffer[writer->length++] = 0;
        --padding;
    }

    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_read_u32(tg_mtproto_tl_reader *reader,
                                            unsigned long *value)
{
    const unsigned char *p;

    if (reader == 0 || reader->buffer == 0 || value == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (reader->offset > reader->length || reader->length - reader->offset < 4UL) {
        return TG_MTPROTO_TL_TRUNCATED;
    }
    p = reader->buffer + reader->offset;
    *value = ((unsigned long)p[0]) |
             (((unsigned long)p[1]) << 8) |
             (((unsigned long)p[2]) << 16) |
             (((unsigned long)p[3]) << 24);
    reader->offset += 4;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_read_u64(tg_mtproto_tl_reader *reader,
                                            unsigned long *hi,
                                            unsigned long *lo)
{
    tg_mtproto_tl_status status;

    if (hi == 0 || lo == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    status = tg_mtproto_tl_read_u32(reader, lo);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return tg_mtproto_tl_read_u32(reader, hi);
}

tg_mtproto_tl_status tg_mtproto_tl_read_raw(tg_mtproto_tl_reader *reader,
                                            const unsigned char **data,
                                            unsigned long data_length)
{
    if (reader == 0 || reader->buffer == 0 || data == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (reader->offset > reader->length ||
        reader->length - reader->offset < data_length) {
        return TG_MTPROTO_TL_TRUNCATED;
    }

    *data = reader->buffer + reader->offset;
    reader->offset += data_length;
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_tl_read_bytes(tg_mtproto_tl_reader *reader,
                                              const unsigned char **data,
                                              unsigned long *data_length)
{
    unsigned long prefix_length;
    unsigned long padding;
    unsigned long total_without_padding;
    unsigned long length;
    const unsigned char *p;

    if (reader == 0 || reader->buffer == 0 || data == 0 || data_length == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (reader->offset >= reader->length) {
        return TG_MTPROTO_TL_TRUNCATED;
    }

    p = reader->buffer + reader->offset;
    if (p[0] < 254U) {
        prefix_length = 1;
        length = (unsigned long)p[0];
    } else {
        if (reader->length - reader->offset < 4UL) {
            return TG_MTPROTO_TL_TRUNCATED;
        }
        prefix_length = 4;
        length = ((unsigned long)p[1]) |
                 (((unsigned long)p[2]) << 8) |
                 (((unsigned long)p[3]) << 16);
    }

    total_without_padding = prefix_length + length;
    padding = (4UL - (total_without_padding % 4UL)) % 4UL;
    if (reader->length - reader->offset < total_without_padding + padding) {
        return TG_MTPROTO_TL_TRUNCATED;
    }

    *data = reader->buffer + reader->offset + prefix_length;
    *data_length = length;
    reader->offset += total_without_padding + padding;
    return TG_MTPROTO_TL_OK;
}

const char *tg_mtproto_tl_status_name(tg_mtproto_tl_status status)
{
    switch (status) {
    case TG_MTPROTO_TL_OK:
        return "ok";
    case TG_MTPROTO_TL_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_MTPROTO_TL_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    case TG_MTPROTO_TL_TRUNCATED:
        return "truncated";
    case TG_MTPROTO_TL_INVALID_DATA:
        return "invalid-data";
    default:
        return "unknown";
    }
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_tl_self_test(void)
{
    static const unsigned char nonce[16] = {
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    static const unsigned char hello[] = { 'h', 'e', 'l', 'l', 'o' };
    static const unsigned char expected_req_pq[] = {
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    unsigned char buffer[64];
    tg_mtproto_tl_writer writer;
    tg_mtproto_tl_reader reader;
    const unsigned char *decoded;
    const unsigned char *raw;
    unsigned long decoded_length;
    unsigned long constructor;
    unsigned long hi;
    unsigned long lo;

    tg_mtproto_tl_writer_init(&writer, buffer, sizeof(buffer));
    if (tg_mtproto_tl_write_u32(&writer, 0xbe7e8ef1UL) != TG_MTPROTO_TL_OK ||
        tg_mtproto_tl_write_raw(&writer, nonce, sizeof(nonce)) != TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected_req_pq) ||
        memcmp(buffer, expected_req_pq, sizeof(expected_req_pq)) != 0) {
        return 2;
    }

    tg_mtproto_tl_reader_init(&reader, buffer, writer.length);
    if (tg_mtproto_tl_read_u32(&reader, &constructor) != TG_MTPROTO_TL_OK ||
        constructor != 0xbe7e8ef1UL ||
        reader.offset != 4UL) {
        return 2;
    }
    if (tg_mtproto_tl_read_raw(&reader, &raw, sizeof(nonce)) !=
            TG_MTPROTO_TL_OK ||
        memcmp(raw, nonce, sizeof(nonce)) != 0 ||
        reader.offset != sizeof(expected_req_pq)) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, buffer, sizeof(buffer));
    if (tg_mtproto_tl_write_u64(&writer, 0x11223344UL, 0x55667788UL) !=
        TG_MTPROTO_TL_OK) {
        return 2;
    }
    tg_mtproto_tl_reader_init(&reader, buffer, writer.length);
    if (tg_mtproto_tl_read_u64(&reader, &hi, &lo) != TG_MTPROTO_TL_OK ||
        hi != 0x11223344UL || lo != 0x55667788UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, buffer, sizeof(buffer));
    if (tg_mtproto_tl_write_bytes(&writer, hello, sizeof(hello)) != TG_MTPROTO_TL_OK ||
        writer.length != 8UL ||
        buffer[0] != 5U ||
        memcmp(buffer + 1, hello, sizeof(hello)) != 0 ||
        buffer[6] != 0U || buffer[7] != 0U) {
        return 2;
    }

    tg_mtproto_tl_reader_init(&reader, buffer, writer.length);
    if (tg_mtproto_tl_read_bytes(&reader, &decoded, &decoded_length) !=
            TG_MTPROTO_TL_OK ||
        decoded_length != sizeof(hello) ||
        memcmp(decoded, hello, sizeof(hello)) != 0 ||
        reader.offset != writer.length) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
