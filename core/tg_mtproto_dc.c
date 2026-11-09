/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_mtproto_dc.h"

static const tg_mtproto_dc_option tg_mtproto_dc_options[] = {
    {1, "pluto", "pluto.web.telegram.org", "149.154.175.50"},
    {2, "venus", "venus.web.telegram.org", "149.154.167.50"},
    {3, "aurora", "aurora.web.telegram.org", "149.154.175.100"},
    {4, "vesta", "vesta.web.telegram.org", "149.154.167.91"},
    {5, "flora", "flora.web.telegram.org", "91.108.56.130"}
};

const tg_mtproto_dc_option *tg_mtproto_dc_by_id(int id)
{
    unsigned int i;

    for (i = 0; i < sizeof(tg_mtproto_dc_options) /
                    sizeof(tg_mtproto_dc_options[0]); ++i) {
        if (tg_mtproto_dc_options[i].id == id) {
            return &tg_mtproto_dc_options[i];
        }
    }

    return 0;
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_dc_self_test(void)
{
    const tg_mtproto_dc_option *dc;

    dc = tg_mtproto_dc_by_id(2);
    if (dc == 0 ||
        dc->id != 2 ||
        strcmp(dc->name, "venus") != 0 ||
        strcmp(dc->web_host, "venus.web.telegram.org") != 0 ||
        strcmp(dc->mt_ip, "149.154.167.50") != 0) {
        return 2;
    }
    dc = tg_mtproto_dc_by_id(5);
    if (dc == 0 || strcmp(dc->mt_ip, "91.108.56.130") != 0) {
        return 2;
    }

    if (tg_mtproto_dc_by_id(0) != 0 ||
        tg_mtproto_dc_by_id(6) != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
