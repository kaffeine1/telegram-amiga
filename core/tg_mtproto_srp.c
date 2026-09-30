/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "tg_mtproto_bigint.h"
#include "tg_mtproto_srp.h"

#define TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR 0x3a912d4aUL
#define TG_SRP_PASSWORD_MAX 256U
#define TG_SRP_PBKDF2_ITERATIONS 100000UL

static int tg_srp_has_nonzero(const unsigned char *data,
                              unsigned long data_length)
{
    unsigned long i;

    if (data == 0 || data_length == 0UL) {
        return 0;
    }
    for (i = 0UL; i < data_length; ++i) {
        if (data[i] != 0U) {
            return 1;
        }
    }
    return 0;
}

static void tg_srp_pad_right(const unsigned char *data,
                             unsigned long data_length,
                             unsigned char out[TG_MTPROTO_SRP_VALUE_LENGTH])
{
    memset(out, 0, TG_MTPROTO_SRP_VALUE_LENGTH);
    if (data != 0 && data_length <= TG_MTPROTO_SRP_VALUE_LENGTH) {
        memcpy(out + TG_MTPROTO_SRP_VALUE_LENGTH - data_length, data,
               (size_t)data_length);
    }
}

static void tg_srp_sha256_salted(const unsigned char *data,
                                 unsigned long data_length,
                                 const unsigned char *salt,
                                 unsigned long salt_length,
                                 unsigned char out[TG_MTPROTO_SHA256_LENGTH])
{
    unsigned char buffer[(TG_MTPROTO_PASSWORD_BYTES_MAX * 2U) +
                         TG_SRP_PASSWORD_MAX];

    memcpy(buffer, salt, (size_t)salt_length);
    memcpy(buffer + salt_length, data, (size_t)data_length);
    memcpy(buffer + salt_length + data_length, salt, (size_t)salt_length);
    tg_mtproto_sha256(buffer, salt_length + data_length + salt_length, out);
}

static tg_mtproto_tl_status tg_srp_derive_x(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    unsigned long pbkdf2_iterations,
    unsigned char x[TG_MTPROTO_SHA256_LENGTH])
{
    unsigned char ph1a[TG_MTPROTO_SHA256_LENGTH];
    unsigned char ph1b[TG_MTPROTO_SHA256_LENGTH];
    unsigned char pbkdf2[TG_MTPROTO_SHA512_LENGTH];

    tg_srp_sha256_salted(password_bytes, password_length,
                         password->current_salt1,
                         password->current_salt1_length, ph1a);
    tg_srp_sha256_salted(ph1a, sizeof(ph1a),
                         password->current_salt2,
                         password->current_salt2_length, ph1b);
    if (tg_mtproto_pbkdf2_hmac_sha512(
            ph1b, sizeof(ph1b),
            password->current_salt1,
            password->current_salt1_length,
            pbkdf2_iterations,
            pbkdf2,
            sizeof(pbkdf2)) != 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_srp_sha256_salted(pbkdf2, sizeof(pbkdf2),
                         password->current_salt2,
                         password->current_salt2_length, x);
    return TG_MTPROTO_TL_OK;
}

/* Everything in the proof that does not depend on Telegram's challenge
   (srp_id, srp_B): the checks on the group, the PBKDF2 derivation of x, k,
   A = g^a and v = g^x, plus the hashes M1 needs. */
static tg_mtproto_tl_status tg_srp_prepare_iterations(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    const unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH],
    unsigned long pbkdf2_iterations,
    tg_mtproto_srp_prepared *out)
{
    unsigned char one[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char k_hash[TG_MTPROTO_SHA256_LENGTH];
    unsigned char hash_input[TG_MTPROTO_SRP_VALUE_LENGTH * 2U];

    if (password == 0 || password_bytes == 0 || random_a == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (password_length > TG_SRP_PASSWORD_MAX ||
        !password->has_current_algo ||
        password->current_algo_constructor !=
            TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR ||
        password->current_g == 0UL ||
        password->current_salt1_length > TG_MTPROTO_PASSWORD_BYTES_MAX ||
        password->current_salt2_length > TG_MTPROTO_PASSWORD_BYTES_MAX ||
        password->current_p_length == 0UL ||
        password->current_p_length > TG_MTPROTO_SRP_VALUE_LENGTH ||
        !tg_srp_has_nonzero(password->current_p, password->current_p_length) ||
        !tg_srp_has_nonzero(random_a, TG_MTPROTO_SRP_VALUE_LENGTH)) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    memset(out, 0, sizeof(*out));
    tg_srp_pad_right(password->current_p, password->current_p_length, out->p);
    tg_mtproto_bigint_from_u32(password->current_g, out->g);
    out->g_value = password->current_g;
    tg_mtproto_bigint_from_u32(1UL, one);
    if (tg_mtproto_bigint_cmp(out->p, one) <= 0 ||
        tg_mtproto_bigint_cmp(out->g, out->p) >= 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    if (tg_srp_derive_x(password, password_bytes, password_length,
                        pbkdf2_iterations, out->x) != TG_MTPROTO_TL_OK) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    memcpy(hash_input, out->p, TG_MTPROTO_SRP_VALUE_LENGTH);
    memcpy(hash_input + TG_MTPROTO_SRP_VALUE_LENGTH, out->g,
           TG_MTPROTO_SRP_VALUE_LENGTH);
    tg_mtproto_sha256(hash_input, TG_MTPROTO_SRP_VALUE_LENGTH * 2UL, k_hash);
    tg_srp_pad_right(k_hash, sizeof(k_hash), out->k);

    memcpy(out->random_a, random_a, TG_MTPROTO_SRP_VALUE_LENGTH);
    tg_mtproto_progress_tick();
    tg_mtproto_bigint_mod_exp(out->g, random_a, TG_MTPROTO_SRP_VALUE_LENGTH,
                              out->p, out->a_public);
    tg_mtproto_progress_tick();
    tg_mtproto_bigint_mod_exp(out->g, out->x, sizeof(out->x), out->p,
                              out->v);

    tg_mtproto_sha256(out->p, sizeof(out->p), out->hp);
    tg_mtproto_sha256(out->g, sizeof(out->g), out->hg);
    tg_mtproto_sha256(password->current_salt1, password->current_salt1_length,
                      out->hs1);
    tg_mtproto_sha256(password->current_salt2, password->current_salt2_length,
                      out->hs2);
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_srp_prepare(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    const unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH],
    tg_mtproto_srp_prepared *out)
{
    return tg_srp_prepare_iterations(password, password_bytes,
                                     password_length, random_a,
                                     TG_SRP_PBKDF2_ITERATIONS, out);
}

int tg_mtproto_srp_same_password(const tg_mtproto_srp_prepared *prepared,
                                 const tg_mtproto_password_summary *password)
{
    unsigned char p[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char hs[TG_MTPROTO_SHA256_LENGTH];

    if (prepared == 0 || password == 0 || !password->has_current_algo ||
        password->current_algo_constructor !=
            TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR ||
        password->current_g != prepared->g_value ||
        password->current_p_length == 0UL ||
        password->current_p_length > TG_MTPROTO_SRP_VALUE_LENGTH ||
        password->current_salt1_length > TG_MTPROTO_PASSWORD_BYTES_MAX ||
        password->current_salt2_length > TG_MTPROTO_PASSWORD_BYTES_MAX) {
        return 0;
    }
    tg_srp_pad_right(password->current_p, password->current_p_length, p);
    if (memcmp(p, prepared->p, sizeof(p)) != 0) {
        return 0;
    }
    tg_mtproto_sha256(password->current_salt1, password->current_salt1_length,
                      hs);
    if (memcmp(hs, prepared->hs1, sizeof(hs)) != 0) {
        return 0;
    }
    tg_mtproto_sha256(password->current_salt2, password->current_salt2_length,
                      hs);
    return memcmp(hs, prepared->hs2, sizeof(hs)) == 0;
}

tg_mtproto_tl_status tg_mtproto_srp_finish(
    const tg_mtproto_srp_prepared *prepared,
    const tg_mtproto_password_summary *password,
    tg_mtproto_srp_proof *out)
{
    unsigned char b[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char u[TG_MTPROTO_SHA256_LENGTH];
    unsigned char kv[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char base[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char exponent[TG_MTPROTO_SRP_EXP_LENGTH];
    unsigned char ux[TG_MTPROTO_SRP_EXP_LENGTH];
    unsigned char s[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char session_key[TG_MTPROTO_SHA256_LENGTH];
    unsigned char hash_input[(TG_MTPROTO_SHA256_LENGTH * 3U) +
                             (TG_MTPROTO_SRP_VALUE_LENGTH * 2U) +
                             TG_MTPROTO_SHA256_LENGTH];
    unsigned long offset;
    unsigned int i;

    if (prepared == 0 || password == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (!tg_mtproto_srp_same_password(prepared, password) ||
        password->srp_b_length == 0UL ||
        password->srp_b_length > TG_MTPROTO_SRP_VALUE_LENGTH ||
        !tg_srp_has_nonzero(password->srp_b, password->srp_b_length)) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    tg_srp_pad_right(password->srp_b, password->srp_b_length, b);
    if (tg_mtproto_bigint_cmp(b, prepared->p) >= 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    memcpy(out->a, prepared->a_public, TG_MTPROTO_SRP_VALUE_LENGTH);
    out->a_length = TG_MTPROTO_SRP_VALUE_LENGTH;

    memcpy(hash_input, out->a, TG_MTPROTO_SRP_VALUE_LENGTH);
    memcpy(hash_input + TG_MTPROTO_SRP_VALUE_LENGTH, b,
           TG_MTPROTO_SRP_VALUE_LENGTH);
    tg_mtproto_sha256(hash_input, TG_MTPROTO_SRP_VALUE_LENGTH * 2UL, u);

    tg_mtproto_bigint_mod_mul(prepared->k, prepared->v, prepared->p, kv);
    tg_mtproto_bigint_sub_mod(base, b, kv, prepared->p);

    tg_mtproto_bigint_mul_bytes(u, sizeof(u), prepared->x, sizeof(prepared->x),
                                ux, sizeof(ux));
    memcpy(exponent, ux, sizeof(exponent));
    tg_mtproto_bigint_add_bytes(exponent, sizeof(exponent),
                                prepared->random_a,
                                TG_MTPROTO_SRP_VALUE_LENGTH);
    tg_mtproto_progress_tick();
    tg_mtproto_bigint_mod_exp(base, exponent, sizeof(exponent), prepared->p,
                              s);
    tg_mtproto_sha256(s, sizeof(s), session_key);

    for (i = 0U; i < TG_MTPROTO_SHA256_LENGTH; ++i) {
        hash_input[i] = (unsigned char)(prepared->hp[i] ^ prepared->hg[i]);
    }
    offset = TG_MTPROTO_SHA256_LENGTH;
    memcpy(hash_input + offset, prepared->hs1, TG_MTPROTO_SHA256_LENGTH);
    offset += TG_MTPROTO_SHA256_LENGTH;
    memcpy(hash_input + offset, prepared->hs2, TG_MTPROTO_SHA256_LENGTH);
    offset += TG_MTPROTO_SHA256_LENGTH;
    memcpy(hash_input + offset, out->a, TG_MTPROTO_SRP_VALUE_LENGTH);
    offset += TG_MTPROTO_SRP_VALUE_LENGTH;
    memcpy(hash_input + offset, b, TG_MTPROTO_SRP_VALUE_LENGTH);
    offset += TG_MTPROTO_SRP_VALUE_LENGTH;
    memcpy(hash_input + offset, session_key, TG_MTPROTO_SHA256_LENGTH);
    offset += TG_MTPROTO_SHA256_LENGTH;
    tg_mtproto_sha256(hash_input, offset, out->m1);

    return TG_MTPROTO_TL_OK;
}

static tg_mtproto_tl_status tg_srp_make_proof_iterations(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    const unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH],
    unsigned long pbkdf2_iterations,
    tg_mtproto_srp_proof *out)
{
    static tg_mtproto_srp_prepared prepared; /* 1.6 KB: off the stack */
    tg_mtproto_tl_status status;

    if (password == 0 || out == 0) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }
    if (password->srp_b_length == 0UL ||
        password->srp_b_length > TG_MTPROTO_SRP_VALUE_LENGTH ||
        !tg_srp_has_nonzero(password->srp_b, password->srp_b_length)) {
        return TG_MTPROTO_TL_INVALID_DATA; /* before the long derivation */
    }
    status = tg_srp_prepare_iterations(password, password_bytes,
                                       password_length, random_a,
                                       pbkdf2_iterations, &prepared);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_srp_finish(&prepared, password, out);
    }
    memset(&prepared, 0, sizeof(prepared));
    return status;
}

tg_mtproto_tl_status tg_mtproto_srp_make_proof(
    const tg_mtproto_password_summary *password,
    const unsigned char *password_bytes,
    unsigned long password_length,
    const unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH],
    tg_mtproto_srp_proof *out)
{
    return tg_srp_make_proof_iterations(password, password_bytes,
                                        password_length, random_a,
                                        TG_SRP_PBKDF2_ITERATIONS, out);
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_srp_self_test(void)
{
    static const unsigned char m1_b15[TG_MTPROTO_SHA256_LENGTH] = {
        0xe0U,0x81U,0x50U,0x39U,0x2dU,0xabU,0x95U,0xb6U,
        0x5bU,0x6bU,0x9cU,0x4fU,0xb6U,0x87U,0xf3U,0x97U,
        0xfdU,0x5bU,0x6fU,0x73U,0x0fU,0x97U,0xb1U,0x29U,
        0x54U,0x0eU,0x08U,0x26U,0x0fU,0xbcU,0x5fU,0x09U
    };
    static const unsigned char m1_b16[TG_MTPROTO_SHA256_LENGTH] = {
        0x5eU,0xa4U,0x85U,0x70U,0xf5U,0x16U,0x6aU,0xcdU,
        0x3dU,0xd9U,0xaaU,0xe2U,0xb3U,0x85U,0x65U,0xbbU,
        0x51U,0x25U,0x43U,0x5fU,0x40U,0xebU,0xc2U,0xb7U,
        0x72U,0x30U,0xeeU,0xc8U,0xedU,0xa6U,0x6fU,0xeeU
    };
    static tg_mtproto_srp_prepared prepared;
    static tg_mtproto_password_summary fresh;
    tg_mtproto_password_summary password;
    tg_mtproto_srp_proof proof;
    unsigned char random_a[TG_MTPROTO_SRP_VALUE_LENGTH];
    unsigned char password_bytes[4];
    unsigned int i;
    int m1_zero;

    memset(&password, 0, sizeof(password));
    password.has_current_algo = 1;
    password.current_algo_constructor = TG_PASSWORD_KDF_ALGO_SRP_CONSTRUCTOR;
    password.current_g = 3UL;
    password.current_salt1[0] = 0x11U;
    password.current_salt1[1] = 0x12U;
    password.current_salt1_length = 2UL;
    password.current_salt2[0] = 0x21U;
    password.current_salt2[1] = 0x22U;
    password.current_salt2[2] = 0x23U;
    password.current_salt2_length = 3UL;
    password.current_p[0] = 17U;
    password.current_p_length = 1UL;
    password.srp_b[0] = 15U;
    password.srp_b_length = 1UL;
    password.srp_id_lo = 1UL;
    memset(random_a, 0, sizeof(random_a));
    random_a[TG_MTPROTO_SRP_VALUE_LENGTH - 1U] = 7U;
    password_bytes[0] = 't';
    password_bytes[1] = 'e';
    password_bytes[2] = 's';
    password_bytes[3] = 't';

    if (tg_srp_make_proof_iterations(&password, password_bytes,
                                     sizeof(password_bytes), random_a,
                                     1UL, &proof) != TG_MTPROTO_TL_OK ||
        proof.a_length != TG_MTPROTO_SRP_VALUE_LENGTH ||
        !tg_srp_has_nonzero(proof.a, sizeof(proof.a))) {
        return 1;
    }
    m1_zero = 1;
    for (i = 0U; i < TG_MTPROTO_SHA256_LENGTH; ++i) {
        if (proof.m1[i] != 0U) {
            m1_zero = 0;
        }
    }
    if (m1_zero) {
        return 1;
    }

    /* The same values the single-step code of 0.0.94 computed for these
       inputs (one PBKDF2 round), with srp_B 15 and 16. */
    if (proof.a[TG_MTPROTO_SRP_VALUE_LENGTH - 1U] != 11U ||
        memcmp(proof.m1, m1_b15, sizeof(m1_b15)) != 0) {
        return 1;
    }
    /* Prepared with one challenge and finished with a fresh one: the proof
       is the one the fresh challenge alone gives. */
    if (tg_srp_prepare_iterations(&password, password_bytes,
                                  sizeof(password_bytes), random_a, 1UL,
                                  &prepared) != TG_MTPROTO_TL_OK) {
        return 1;
    }
    fresh = password;
    fresh.srp_b[0] = 16U;
    fresh.srp_id_lo = 2UL;
    if (!tg_mtproto_srp_same_password(&prepared, &fresh) ||
        tg_mtproto_srp_finish(&prepared, &fresh, &proof) != TG_MTPROTO_TL_OK ||
        memcmp(proof.m1, m1_b16, sizeof(m1_b16)) != 0) {
        return 1;
    }
    /* A password changed in the meantime (new salt) is told apart. */
    fresh.current_salt1[0] = 0x99U;
    if (tg_mtproto_srp_same_password(&prepared, &fresh) ||
        tg_mtproto_srp_finish(&prepared, &fresh, &proof) !=
            TG_MTPROTO_TL_INVALID_DATA) {
        return 1;
    }
    memset(&prepared, 0, sizeof(prepared));

    password.current_algo_constructor = 0UL;
    if (tg_srp_make_proof_iterations(&password, password_bytes,
                                     sizeof(password_bytes), random_a,
                                     1UL, &proof) !=
        TG_MTPROTO_TL_INVALID_DATA) {
        return 1;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
