/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_MTPROTO_RSA_H
#define TG_MTPROTO_RSA_H

#include "tg_mtproto_auth.h"
#include "tg_mtproto_tl.h"

#define TG_MTPROTO_RSA_MODULUS_LENGTH 256U
#define TG_MTPROTO_RSA_PADDED_LENGTH 256U
#define TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX 1024U
#define TG_MTPROTO_DH_VALUE_MAX 256U
/* 256-bit private DH exponent for MTProto auth-key bootstrap. The public
   values remain full-size modulo Telegram's 2048-bit safe prime; this keeps
   roughly 128-bit discrete-log strength while avoiding very slow 2048-bit
   exponentiation on classic Amiga targets. */
#define TG_MTPROTO_DH_PRIVATE_EXPONENT_BYTES 32U
#define TG_MTPROTO_AUTH_KEY_LENGTH 256U

typedef struct tg_mtproto_public_key {
    tg_mtproto_fingerprint fingerprint;
    unsigned char modulus[TG_MTPROTO_RSA_MODULUS_LENGTH];
    unsigned long exponent;
} tg_mtproto_public_key;

typedef struct tg_mtproto_server_dh_params_ok {
    unsigned char nonce[16];
    unsigned char server_nonce[16];
    unsigned char encrypted_answer[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX];
    unsigned long encrypted_answer_length;
} tg_mtproto_server_dh_params_ok;

typedef struct tg_mtproto_server_dh_inner_data {
    unsigned char nonce[16];
    unsigned char server_nonce[16];
    unsigned long g;
    unsigned char dh_prime[TG_MTPROTO_DH_VALUE_MAX];
    unsigned long dh_prime_length;
    unsigned char g_a[TG_MTPROTO_DH_VALUE_MAX];
    unsigned long g_a_length;
    unsigned long server_time;
} tg_mtproto_server_dh_inner_data;

typedef struct tg_mtproto_set_client_dh_answer {
    unsigned long constructor;
    unsigned char nonce[16];
    unsigned char server_nonce[16];
    unsigned char new_nonce_hash[16];
} tg_mtproto_set_client_dh_answer;

const tg_mtproto_public_key *tg_mtproto_builtin_public_keys(
    unsigned int *count);

const tg_mtproto_public_key *tg_mtproto_select_public_key(
    const tg_mtproto_res_pq *res_pq);

tg_mtproto_tl_status tg_mtproto_build_p_q_inner_data_dc(
    tg_mtproto_tl_writer *writer,
    const unsigned char *pq,
    unsigned long pq_length,
    const unsigned char *p,
    unsigned long p_length,
    const unsigned char *q,
    unsigned long q_length,
    const unsigned char nonce[16],
    const unsigned char server_nonce[16],
    const unsigned char new_nonce[32],
    long dc_id);

tg_mtproto_tl_status tg_mtproto_rsa_pad(
    const unsigned char *data,
    unsigned long data_length,
    const unsigned char random_padding[96],
    const unsigned char temp_key[32],
    const tg_mtproto_public_key *public_key,
    unsigned char encrypted_data[TG_MTPROTO_RSA_PADDED_LENGTH]);

tg_mtproto_tl_status tg_mtproto_build_req_dh_params(
    tg_mtproto_tl_writer *writer,
    const unsigned char nonce[16],
    const unsigned char server_nonce[16],
    const unsigned char *p,
    unsigned long p_length,
    const unsigned char *q,
    unsigned long q_length,
    const tg_mtproto_fingerprint *fingerprint,
    const unsigned char encrypted_data[TG_MTPROTO_RSA_PADDED_LENGTH]);

tg_mtproto_tl_status tg_mtproto_parse_server_dh_params_ok(
    const unsigned char *payload,
    unsigned long payload_length,
    tg_mtproto_server_dh_params_ok *out);

tg_mtproto_tl_status tg_mtproto_decrypt_server_dh_inner_data(
    const unsigned char *encrypted_answer,
    unsigned long encrypted_answer_length,
    const unsigned char new_nonce[32],
    const unsigned char expected_nonce[16],
    const unsigned char expected_server_nonce[16],
    tg_mtproto_server_dh_inner_data *out);

int tg_mtproto_check_dh_params(const tg_mtproto_server_dh_inner_data *inner);

tg_mtproto_tl_status tg_mtproto_build_set_client_dh_params(
    tg_mtproto_tl_writer *writer,
    const unsigned char nonce[16],
    const unsigned char server_nonce[16],
    const unsigned char *encrypted_data,
    unsigned long encrypted_data_length);

tg_mtproto_tl_status tg_mtproto_build_client_dh_request(
    const tg_mtproto_server_dh_inner_data *inner,
    const unsigned char new_nonce[32],
    const unsigned char b[TG_MTPROTO_DH_VALUE_MAX],
    const unsigned char padding[15],
    unsigned char encrypted_data[TG_MTPROTO_DH_ENCRYPTED_ANSWER_MAX],
    unsigned long *encrypted_data_length,
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH]);

tg_mtproto_tl_status tg_mtproto_parse_set_client_dh_answer(
    const unsigned char *payload,
    unsigned long payload_length,
    tg_mtproto_set_client_dh_answer *out);

int tg_mtproto_verify_dh_gen_ok(const tg_mtproto_set_client_dh_answer *answer,
                                const unsigned char expected_nonce[16],
                                const unsigned char expected_server_nonce[16],
                                const unsigned char new_nonce[32],
                                const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH]);

/* One run of Pollard's rho (Brent's cycle) on an odd n below 2^64 with the
   polynomial x^2 + c: a factor of n, or 0 when this c finds none. */
unsigned long long tg_mtproto_pq_rho(unsigned long long n,
                                     unsigned long long c);

int tg_mtproto_rsa_self_test(void);

#endif
