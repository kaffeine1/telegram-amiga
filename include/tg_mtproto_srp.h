/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_MTPROTO_SRP_H
#define TG_MTPROTO_SRP_H

#include "tg_mtproto_crypto.h"
#include "tg_mtproto_login.h"
#include "tg_mtproto_tl.h"

#define TG_MTPROTO_SRP_VALUE_LENGTH 256U
#define TG_MTPROTO_SRP_EXP_LENGTH 512U

/* Bytes of randomness in the client's private SRP exponent 'a'. Only g^a (a
   full-size residue) is sent, so a's length is the client's choice: a 256-bit
   'a' keeps standard SRP security yet shrinks the g^a and base^(a+u*x) modexps
   from ~2048-bit to ~256-/~512-bit exponents -- a big 2FA speed-up on m68k.
   Mirrors the DH handshake's TG_MTPROTO_DH_PRIVATE_EXPONENT_BYTES. */
#define TG_MTPROTO_SRP_PRIVATE_EXPONENT_BYTES 32U

typedef struct tg_mtproto_srp_proof {
    unsigned char a[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned long a_length;
    unsigned char m1[TG_MTPROTO_SHA256_LENGTH];
} tg_mtproto_srp_proof;

tg_mtproto_tl_status tg_mtproto_srp_make_proof(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    const unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH],
    tg_mtproto_srp_proof *out);

/* The proof in two steps (0.0.95). Everything that depends only on the
   password and on the account's salts and group comes first: the PBKDF2
   derivation (some forty minutes on a stock 14 MHz 68020), A = g^a and
   v = g^x. Telegram's challenge (srp_id, srp_B) expires long before such a
   wait ends, so the client then asks account.getPassword again and
   finishes with the fresh challenge: one exponentiation and some hashes.
   The prepared state holds secrets (x, a): zero it after use. */
typedef struct tg_mtproto_srp_prepared {
    unsigned char p[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char g[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char k[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char v[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char a_public[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char x[TG_MTPROTO_SHA256_LENGTH];
    unsigned char hp[TG_MTPROTO_SHA256_LENGTH];
    unsigned char hg[TG_MTPROTO_SHA256_LENGTH];
    unsigned char hs1[TG_MTPROTO_SHA256_LENGTH];
    unsigned char hs2[TG_MTPROTO_SHA256_LENGTH];
    unsigned long g_value;
} tg_mtproto_srp_prepared;

tg_mtproto_tl_status tg_mtproto_srp_prepare(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    const unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH],
    tg_mtproto_srp_prepared *out);

/* 1 when `password` still carries the salts and the group the preparation
   used: 0 means the account's password changed in the meantime. */
int tg_mtproto_srp_same_password(const tg_mtproto_srp_prepared *prepared,
                                 const tg_mtproto_password_summary *password);

/* The proof for the challenge in `password`, which must carry the same
   salts and group as the preparation. */
tg_mtproto_tl_status tg_mtproto_srp_finish(
    const tg_mtproto_srp_prepared *prepared,
    const tg_mtproto_password_summary *password,
    tg_mtproto_srp_proof *out);

int tg_mtproto_srp_self_test(void);

#endif
