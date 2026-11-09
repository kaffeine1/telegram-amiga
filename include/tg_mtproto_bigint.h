/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_MTPROTO_BIGINT_H
#define TG_MTPROTO_BIGINT_H

#define TG_MTPROTO_BIGINT_SIZE 256U
#define TG_MTPROTO_BIGINT_EXP_MAX 512U

int tg_mtproto_bigint_cmp(
    const unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_sub(
    unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_add_mod(
    unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_sub_mod(
    unsigned char out[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_mod_mul(
    const unsigned char a[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char b[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE],
    unsigned char out[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_mod_exp(
    const unsigned char base[TG_MTPROTO_BIGINT_SIZE],
    const unsigned char *exponent,
    unsigned long exponent_length,
    const unsigned char modulus[TG_MTPROTO_BIGINT_SIZE],
    unsigned char output[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_from_u32(
    unsigned long value,
    unsigned char out[TG_MTPROTO_BIGINT_SIZE]);
void tg_mtproto_bigint_mul_bytes(
    const unsigned char *a,
    unsigned long a_length,
    const unsigned char *b,
    unsigned long b_length,
    unsigned char *out,
    unsigned long out_length);
void tg_mtproto_bigint_add_bytes(
    unsigned char *a,
    unsigned long a_length,
    const unsigned char *b,
    unsigned long b_length);
unsigned long tg_mtproto_bigint_trim(
    const unsigned char *data,
    unsigned long data_length);
int tg_mtproto_bigint_self_test(void);

#endif
