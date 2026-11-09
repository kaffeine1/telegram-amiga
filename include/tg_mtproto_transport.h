/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_TRANSPORT_H
#define TG_MTPROTO_TRANSPORT_H

#include "tg_mtproto_tl.h"

tg_mtproto_tl_status tg_mtproto_write_abridged_init(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_write_abridged_packet(
    tg_mtproto_tl_writer *writer,
    const unsigned char *payload,
    unsigned long payload_length);

tg_mtproto_tl_status tg_mtproto_write_intermediate_init(
    tg_mtproto_tl_writer *writer);

tg_mtproto_tl_status tg_mtproto_write_intermediate_packet(
    tg_mtproto_tl_writer *writer,
    const unsigned char *payload,
    unsigned long payload_length);

int tg_mtproto_transport_self_test(void);

#endif
