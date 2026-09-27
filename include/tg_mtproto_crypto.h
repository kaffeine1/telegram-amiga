/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_MTPROTO_CRYPTO_H
#define TG_MTPROTO_CRYPTO_H

#include <stdio.h>

#define TG_MTPROTO_SHA1_LENGTH 20
#define TG_MTPROTO_SHA256_LENGTH 32
#define TG_MTPROTO_SHA512_LENGTH 64

void tg_mtproto_sha1(const unsigned char *data, unsigned long data_length,
                     unsigned char digest[TG_MTPROTO_SHA1_LENGTH]);
void tg_mtproto_sha256(const unsigned char *data, unsigned long data_length,
                       unsigned char digest[TG_MTPROTO_SHA256_LENGTH]);
void tg_mtproto_sha512(const unsigned char *data, unsigned long data_length,
                       unsigned char digest[TG_MTPROTO_SHA512_LENGTH]);
void tg_mtproto_hmac_sha512(const unsigned char *key,
                            unsigned long key_length,
                            const unsigned char *data,
                            unsigned long data_length,
                            unsigned char digest[TG_MTPROTO_SHA512_LENGTH]);
int tg_mtproto_pbkdf2_hmac_sha512(const unsigned char *password,
                                  unsigned long password_length,
                                  const unsigned char *salt,
                                  unsigned long salt_length,
                                  unsigned long iterations,
                                  unsigned char *output,
                                  unsigned long output_length);
void tg_mtproto_aes256_ige_encrypt(unsigned char *data,
                                   unsigned long length,
                                   const unsigned char key[32],
                                   const unsigned char iv[32]);
void tg_mtproto_aes256_ige_decrypt(unsigned char *data,
                                   unsigned long length,
                                   const unsigned char key[32],
                                   const unsigned char iv[32]);
int tg_mtproto_crypto_self_test(void);
int tg_mtproto_aes_self_test(void);
int tg_mtproto_aes_bench(FILE *stream);

/*
 * Optional progress hook for long CPU-bound crypto (PBKDF2/SRP on slow CPUs
 * such as 68080). When a hook is installed, tg_mtproto_progress_tick() is
 * called periodically so callers can animate a loader. The hook is a global,
 * single-threaded opt-in; pass 0 to clear it.
 */
void tg_mtproto_set_progress_hook(void (*hook)(void));
void tg_mtproto_progress_tick(void);

#endif
