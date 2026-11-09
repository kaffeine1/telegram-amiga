/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_DC_H
#define TG_MTPROTO_DC_H

typedef struct tg_mtproto_dc_option {
    int id;
    const char *name;
    const char *web_host;
    /* Production MTProto IPv4 (port 443), for the multi-DC file channel:
       a foreign document downloads from ITS datacenter (0.0.8 punto 1c).
       These addresses have been stable for many years and are the same
       first-hop table every official client ships. */
    const char *mt_ip;
} tg_mtproto_dc_option;

const tg_mtproto_dc_option *tg_mtproto_dc_by_id(int id);
int tg_mtproto_dc_self_test(void);

#endif
