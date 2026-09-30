/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_MTPROTO_AUTH_H
#define TG_MTPROTO_AUTH_H

#define TG_MTPROTO_AUTH_MAX_FINGERPRINTS 8U

#include "tg_mtproto_tl.h"

typedef struct tg_mtproto_fingerprint {
    unsigned long hi;
    unsigned long lo;
} tg_mtproto_fingerprint;

typedef struct tg_mtproto_res_pq {
    unsigned char nonce[16];
    unsigned char server_nonce[16];
    unsigned char pq[16];
    unsigned long pq_length;
    tg_mtproto_fingerprint fingerprints[TG_MTPROTO_AUTH_MAX_FINGERPRINTS];
    unsigned int fingerprint_count;
} tg_mtproto_res_pq;

tg_mtproto_tl_status tg_mtproto_parse_res_pq(
    const unsigned char *payload,
    unsigned long payload_length,
    tg_mtproto_res_pq *out);

int tg_mtproto_res_pq_nonce_matches(const tg_mtproto_res_pq *res_pq,
                                    const unsigned char nonce[16]);

int tg_mtproto_pq_factor(const unsigned char *pq,
                         unsigned long pq_length,
                         unsigned long *p,
                         unsigned long *q);

const tg_mtproto_fingerprint *tg_mtproto_select_fingerprint(
    const tg_mtproto_res_pq *res_pq,
    const tg_mtproto_fingerprint *known,
    unsigned int known_count);

int tg_mtproto_auth_self_test(void);

#if !defined(TG_NO_SELFTEST)
/* The rho the key exchange ran before 0.0.95 (64 shift-and-add rounds per
   multiplication and a division per step), kept as the reference for the
   self-test and the bench. */
unsigned long long tg_mtproto_pq_rho_ref(unsigned long long n,
                                         unsigned long long c);
#endif

#endif
