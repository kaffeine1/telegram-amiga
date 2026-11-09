/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_MESSAGE_ID_H
#define TG_MTPROTO_MESSAGE_ID_H

typedef struct tg_mtproto_message_id {
    unsigned long hi;
    unsigned long lo;
} tg_mtproto_message_id;

void tg_mtproto_client_message_id(unsigned long unix_time,
                                  unsigned long fractional,
                                  const tg_mtproto_message_id *last,
                                  tg_mtproto_message_id *out);

int tg_mtproto_message_id_self_test(void);

#endif
