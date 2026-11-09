/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tg_mtproto_message_id.h"

void tg_mtproto_client_message_id(unsigned long unix_time,
                                  unsigned long fractional,
                                  const tg_mtproto_message_id *last,
                                  tg_mtproto_message_id *out)
{
    tg_mtproto_message_id current;

    if (out == 0) {
        return;
    }

    current.hi = unix_time;
    current.lo = fractional & 0xfffffffcUL;
    if (current.lo == 0) {
        current.lo = 4UL;
    }

    if (last != 0 &&
        (current.hi < last->hi ||
         (current.hi == last->hi && current.lo <= last->lo))) {
        current.hi = last->hi;
        current.lo = (last->lo + 4UL) & 0xfffffffcUL;
        if (current.lo == 0) {
            ++current.hi;
            current.lo = 4UL;
        }
    }

    out->hi = current.hi;
    out->lo = current.lo;
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_message_id_self_test(void)
{
    tg_mtproto_message_id last;
    tg_mtproto_message_id out;

    tg_mtproto_client_message_id(0x6777e5ebUL, 0x00059763UL, 0, &out);
    if (out.hi != 0x6777e5ebUL || out.lo != 0x00059760UL ||
        (out.lo % 4UL) != 0 || out.lo == 0) {
        return 2;
    }

    last.hi = 0x6777e5ebUL;
    last.lo = 0x00059760UL;
    tg_mtproto_client_message_id(0x6777e5ebUL, 0x00059760UL, &last, &out);
    if (out.hi != 0x6777e5ebUL || out.lo != 0x00059764UL) {
        return 2;
    }

    last.hi = 0x6777e5ebUL;
    last.lo = 0xfffffffcUL;
    tg_mtproto_client_message_id(0x6777e5ebUL, 4UL, &last, &out);
    if (out.hi != 0x6777e5ecUL || out.lo != 4UL) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
