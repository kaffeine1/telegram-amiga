/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef TG_MTPROTO_ENCRYPTED_H
#define TG_MTPROTO_ENCRYPTED_H

#include "tg_mtproto_rsa.h"
#include "tg_mtproto_tl.h"

/* Max size of a DECRYPTED MTProto message body. getHistory/dialogs bodies
   arrive gzip-compressed so they fit small; getFile chunks are RAW file bytes
   with no compression, so this must hold a whole download chunk (+ the
   upload.file wrapper and message envelope) or the decrypt rejects it and the
   transfer reads as "no reply". PPC/AROS have RAM to spare -> 72 KB (a 64 KB
   chunk); m68k uses 40 KB for a 32 KB chunk -- big enough that a 16 MB file is
   ~512 round-trips instead of ~2000 (an interminable, machine-locking download
   on a Vampire), still modest static RAM for a stock A1200. */
#if defined(__m68k__)
#ifndef TG_MTPROTO_ENCRYPTED_BODY_MAX /* overridable: the 68000 LOWMEM build
                                         trades body size for fast RAM */
#define TG_MTPROTO_ENCRYPTED_BODY_MAX 40960U
#endif
#else
#ifndef TG_MTPROTO_ENCRYPTED_BODY_MAX /* overridable here too: a host sanitizer
                                         run can wear the 68000 profile */
#define TG_MTPROTO_ENCRYPTED_BODY_MAX 73728U
#endif
#endif

typedef struct tg_mtproto_encrypted_message {
    unsigned long server_salt_hi;
    unsigned long server_salt_lo;
    unsigned char session_id[8];
    unsigned long message_id_hi;
    unsigned long message_id_lo;
    unsigned long seq_no;
    unsigned char body[TG_MTPROTO_ENCRYPTED_BODY_MAX];
    unsigned long body_length;
} tg_mtproto_encrypted_message;

void tg_mtproto_auth_key_id(
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    unsigned long *hi,
    unsigned long *lo);

void tg_mtproto_initial_server_salt(
    const unsigned char new_nonce[32],
    const unsigned char server_nonce[16],
    unsigned long *hi,
    unsigned long *lo);

tg_mtproto_tl_status tg_mtproto_write_encrypted_message(
    tg_mtproto_tl_writer *writer,
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    unsigned long server_salt_hi,
    unsigned long server_salt_lo,
    const unsigned char session_id[8],
    unsigned long message_id_hi,
    unsigned long message_id_lo,
    unsigned long seq_no,
    const unsigned char *body,
    unsigned long body_length,
    const unsigned char *padding,
    unsigned long padding_length);

tg_mtproto_tl_status tg_mtproto_decrypt_encrypted_message(
    const unsigned char *payload,
    unsigned long payload_length,
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    tg_mtproto_encrypted_message *out);

int tg_mtproto_encrypted_self_test(void);

#endif
