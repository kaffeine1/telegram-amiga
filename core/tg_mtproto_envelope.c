/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_mtproto_envelope.h"

tg_mtproto_tl_status tg_mtproto_write_plain_message(
    tg_mtproto_tl_writer *writer,
    unsigned long message_id_hi,
    unsigned long message_id_lo,
    const unsigned char *payload,
    unsigned long payload_length)
{
    tg_mtproto_tl_status status;

    if (payload == 0 && payload_length > 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    status = tg_mtproto_tl_write_u64(writer, 0, 0);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    status = tg_mtproto_tl_write_u64(writer, message_id_hi, message_id_lo);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    status = tg_mtproto_tl_write_u32(writer, payload_length);
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    return tg_mtproto_tl_write_raw(writer, payload, payload_length);
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_envelope_self_test(void)
{
    static const unsigned char req_pq_multi[] = {
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    static const unsigned char expected[] = {
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U,
        0x14U, 0x00U, 0x00U, 0x00U,
        0xf1U, 0x8eU, 0x7eU, 0xbeU,
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
    };
    unsigned char buffer[64];
    tg_mtproto_tl_writer writer;

    tg_mtproto_tl_writer_init(&writer, buffer, sizeof(buffer));
    if (tg_mtproto_write_plain_message(&writer, 0x11223344UL, 0x55667788UL,
                                       req_pq_multi, sizeof(req_pq_multi)) !=
            TG_MTPROTO_TL_OK ||
        writer.length != sizeof(expected) ||
        memcmp(buffer, expected, sizeof(expected)) != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
