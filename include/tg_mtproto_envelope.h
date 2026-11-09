/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_ENVELOPE_H
#define TG_MTPROTO_ENVELOPE_H

#include "tg_mtproto_tl.h"

tg_mtproto_tl_status tg_mtproto_write_plain_message(
    tg_mtproto_tl_writer *writer,
    unsigned long message_id_hi,
    unsigned long message_id_lo,
    const unsigned char *payload,
    unsigned long payload_length);

int tg_mtproto_envelope_self_test(void);

#endif
