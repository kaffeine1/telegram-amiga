/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_mtproto_transport.h"

tg_mtproto_tl_status tg_mtproto_write_abridged_init(
    tg_mtproto_tl_writer *writer)
{
    return tg_mtproto_tl_write_u8(writer, 0xefUL);
}

tg_mtproto_tl_status tg_mtproto_write_abridged_packet(
    tg_mtproto_tl_writer *writer,
    const unsigned char *payload,
    unsigned long payload_length)
{
    unsigned long length_words;
    tg_mtproto_tl_status status;

    if ((payload == 0 && payload_length > 0) || (payload_length % 4UL) != 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    length_words = payload_length / 4UL;
    if (length_words >= 0x1000000UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    if (length_words < 0x7fUL) {
        status = tg_mtproto_tl_write_u8(writer, length_words);
    } else {
        status = tg_mtproto_tl_write_u8(writer, 0x7fUL);
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_write_u8(writer, length_words & 0xffUL);
        }
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_write_u8(writer, (length_words >> 8) & 0xffUL);
        }
        if (status == TG_MTPROTO_TL_OK) {
            status = tg_mtproto_tl_write_u8(writer, (length_words >> 16) & 0xffUL);
        }
    }
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return tg_mtproto_tl_write_raw(writer, payload, payload_length);
}

tg_mtproto_tl_status tg_mtproto_write_intermediate_init(
    tg_mtproto_tl_writer *writer)
{
    return tg_mtproto_tl_write_u32(writer, 0xeeeeeeeeUL);
}

tg_mtproto_tl_status tg_mtproto_write_intermediate_packet(
    tg_mtproto_tl_writer *writer,
    const unsigned char *payload,
    unsigned long payload_length)
{
    tg_mtproto_tl_status status;

    if ((payload == 0 && payload_length > 0) || (payload_length % 4UL) != 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    status = tg_mtproto_tl_write_u32(writer, payload_length);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return tg_mtproto_tl_write_raw(writer, payload, payload_length);
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_transport_self_test(void)
{
    static const unsigned char payload[] = {
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U,
        0x14U, 0x00U, 0x00U, 0x00U,
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    static const unsigned char expected_abridged[] = {
        0xefU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U,
        0x14U, 0x00U, 0x00U, 0x00U,
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    static const unsigned char expected_intermediate[] = {
        0xeeU, 0xeeU, 0xeeU, 0xeeU,
        0x28U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U,
        0x14U, 0x00U, 0x00U, 0x00U,
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    unsigned char buffer[80];
    tg_mtproto_tl_writer writer;

    tg_mtproto_tl_writer_init(&writer, buffer, sizeof(buffer));
    if (tg_mtproto_write_abridged_init(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_write_abridged_packet(&writer, payload, sizeof(payload)) !=
            TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected_abridged) ||
        memcmp(buffer, expected_abridged, sizeof(expected_abridged)) != 0) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, buffer, sizeof(buffer));
    if (tg_mtproto_write_intermediate_init(&writer) != TG_MTPROTO_TL_OK ||
        tg_mtproto_write_intermediate_packet(&writer, payload, sizeof(payload)) !=
            TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected_intermediate) ||
        memcmp(buffer, expected_intermediate, sizeof(expected_intermediate)) != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
