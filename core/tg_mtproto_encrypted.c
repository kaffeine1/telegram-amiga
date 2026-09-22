/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "tg_mtproto_crypto.h"
#include "tg_mtproto_encrypted.h"

/* Size of one MTProto message's AES plaintext (envelope + body + padding), used
   for the intermediate decrypt/encrypt buffers below. It MUST track
   TG_MTPROTO_ENCRYPTED_BODY_MAX: a getFile reply whose body fits out->body[]
   still trips the sizeof(decrypted) guard in decrypt_encrypted_message_x if this
   is smaller. This was the OS4/MorphOS/AROS file-download failure -- the body[]
   buffer was raised to 72 KB for a 64 KB chunk, but this intermediate buffer
   stayed at 12 KB, so every full-chunk reply (encrypted ~66 KB) was rejected as
   "no reply @off 0" while m68k's 8 KB chunks slipped under 12 KB and worked.
   (Same lesson as the body[] fix: size EVERY buffer in the recv->decrypt chain.)
   It bit a SECOND time when m68k went to a 32 KB chunk and only body[] was
   raised -- same "no reply @off 0" on the first chunk. So stop hand-mirroring
   the two numbers and DERIVE this one: it can no longer drift from body[]. */
#define TG_MTPROTO_ENCRYPTED_PACKET_MAX TG_MTPROTO_ENCRYPTED_BODY_MAX

static unsigned long tg_read_le32(const unsigned char *data)
{
    return ((unsigned long)data[0]) |
           (((unsigned long)data[1]) << 8) |
           (((unsigned long)data[2]) << 16) |
           (((unsigned long)data[3]) << 24);
}

#if !defined(TG_NO_SELFTEST)
static void tg_store_le32(unsigned char *data, unsigned long value)
{
    data[0] = (unsigned char)(value & 0xffUL);
    data[1] = (unsigned char)((value >> 8) & 0xffUL);
    data[2] = (unsigned char)((value >> 16) & 0xffUL);
    data[3] = (unsigned char)((value >> 24) & 0xffUL);
}
#endif /* !TG_NO_SELFTEST */

static void tg_mtproto_aes_key_iv(
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    const unsigned char msg_key[16],
    unsigned int x,
    unsigned char aes_key[32],
    unsigned char aes_iv[32])
{
    unsigned char input[68];
    unsigned char sha256_a[TG_MTPROTO_SHA256_LENGTH];
    unsigned char sha256_b[TG_MTPROTO_SHA256_LENGTH];

    memcpy(input, msg_key, 16U);
    memcpy(input + 16U, auth_key + x, 36U);
    tg_mtproto_sha256(input, 52UL, sha256_a);

    memcpy(input, auth_key + 40U + x, 36U);
    memcpy(input + 36U, msg_key, 16U);
    tg_mtproto_sha256(input, 52UL, sha256_b);

    memcpy(aes_key, sha256_a, 8U);
    memcpy(aes_key + 8U, sha256_b + 8U, 16U);
    memcpy(aes_key + 24U, sha256_a + 24U, 8U);

    memcpy(aes_iv, sha256_b, 8U);
    memcpy(aes_iv + 8U, sha256_a + 8U, 16U);
    memcpy(aes_iv + 24U, sha256_b + 24U, 8U);
}

static void tg_mtproto_msg_key(
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    const unsigned char *plaintext,
    unsigned long plaintext_length,
    unsigned int x,
    unsigned char msg_key[16])
{
    static unsigned char input[32U + TG_MTPROTO_ENCRYPTED_PACKET_MAX];
    unsigned char digest[TG_MTPROTO_SHA256_LENGTH];

    memcpy(input, auth_key + 88U + x, 32U);
    memcpy(input + 32U, plaintext, (size_t)plaintext_length);
    tg_mtproto_sha256(input, 32UL + plaintext_length, digest);
    memcpy(msg_key, digest + 8U, 16U);
}

void tg_mtproto_auth_key_id(
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    unsigned long *hi,
    unsigned long *lo)
{
    unsigned char digest[TG_MTPROTO_SHA1_LENGTH];

    if (hi != 0) {
        *hi = 0;
    }
    if (lo != 0) {
        *lo = 0;
    }
    if (auth_key == 0 || hi == 0 || lo == 0) {
        return;
    }
    tg_mtproto_sha1(auth_key, TG_MTPROTO_AUTH_KEY_LENGTH, digest);
    *lo = tg_read_le32(digest + 12U);
    *hi = tg_read_le32(digest + 16U);
}

void tg_mtproto_initial_server_salt(
    const unsigned char new_nonce[32],
    const unsigned char server_nonce[16],
    unsigned long *hi,
    unsigned long *lo)
{
    unsigned char bytes[8];
    unsigned int i;

    if (hi != 0) {
        *hi = 0;
    }
    if (lo != 0) {
        *lo = 0;
    }
    if (new_nonce == 0 || server_nonce == 0 || hi == 0 || lo == 0) {
        return;
    }
    for (i = 0U; i < 8U; ++i) {
        bytes[i] = (unsigned char)(new_nonce[i] ^ server_nonce[i]);
    }
    *lo = tg_read_le32(bytes);
    *hi = tg_read_le32(bytes + 4U);
}

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
    unsigned long padding_length)
{
    static unsigned char plaintext[TG_MTPROTO_ENCRYPTED_PACKET_MAX];
    unsigned char msg_key[16];
    unsigned char aes_key[32];
    unsigned char aes_iv[32];
    unsigned long auth_key_id_hi;
    unsigned long auth_key_id_lo;
    unsigned long plaintext_length;
    tg_mtproto_tl_writer plain_writer;
    tg_mtproto_tl_status status;

    if (writer == 0 || auth_key == 0 || session_id == 0 ||
        (body == 0 && body_length > 0UL) ||
        padding == 0 || padding_length < 12UL ||
        padding_length > 1024UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    plaintext_length = 32UL + body_length + padding_length;
    if (plaintext_length > sizeof(plaintext) ||
        (plaintext_length % 16UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    tg_mtproto_tl_writer_init(&plain_writer, plaintext, sizeof(plaintext));
    status = tg_mtproto_tl_write_u64(&plain_writer, server_salt_hi,
                                     server_salt_lo);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(&plain_writer, session_id, 8UL);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u64(&plain_writer, message_id_hi,
                                         message_id_lo);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(&plain_writer, seq_no);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_u32(&plain_writer, body_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(&plain_writer, body, body_length);
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(&plain_writer, padding,
                                         padding_length);
    }
    if (status != TG_MTPROTO_TL_OK) {
        return status;
    }
    if (plain_writer.length != plaintext_length) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    tg_mtproto_msg_key(auth_key, plaintext, plaintext_length, 0U, msg_key);
    tg_mtproto_aes_key_iv(auth_key, msg_key, 0U, aes_key, aes_iv);
    tg_mtproto_aes256_ige_encrypt(plaintext, plaintext_length, aes_key, aes_iv);
    tg_mtproto_auth_key_id(auth_key, &auth_key_id_hi, &auth_key_id_lo);

    status = tg_mtproto_tl_write_u64(writer, auth_key_id_hi, auth_key_id_lo);
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, msg_key, sizeof(msg_key));
    }
    if (status == TG_MTPROTO_TL_OK) {
        status = tg_mtproto_tl_write_raw(writer, plaintext, plaintext_length);
    }
    return status;
}

static tg_mtproto_tl_status tg_mtproto_decrypt_encrypted_message_x(
    const unsigned char *payload,
    unsigned long payload_length,
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    unsigned int x,
    tg_mtproto_encrypted_message *out)
{
    static unsigned char decrypted[TG_MTPROTO_ENCRYPTED_PACKET_MAX];
    unsigned char expected_msg_key[16];
    unsigned char aes_key[32];
    unsigned char aes_iv[32];
    unsigned long expected_auth_key_hi;
    unsigned long expected_auth_key_lo;
    unsigned long auth_key_hi;
    unsigned long auth_key_lo;
    unsigned long encrypted_length;
    unsigned long padding_length;

    if (payload == 0 || auth_key == 0 || out == 0 ||
        payload_length < 40UL ||
        payload_length - 24UL > sizeof(decrypted) ||
        ((payload_length - 24UL) % 16UL) != 0UL) {
        return TG_MTPROTO_TL_INVALID_ARGUMENT;
    }

    auth_key_lo = tg_read_le32(payload);
    auth_key_hi = tg_read_le32(payload + 4U);
    tg_mtproto_auth_key_id(auth_key, &expected_auth_key_hi,
                           &expected_auth_key_lo);
    if (auth_key_hi != expected_auth_key_hi ||
        auth_key_lo != expected_auth_key_lo) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    encrypted_length = payload_length - 24UL;
    memcpy(decrypted, payload + 24U, (size_t)encrypted_length);
    tg_mtproto_aes_key_iv(auth_key, payload + 8U, x, aes_key, aes_iv);
    tg_mtproto_aes256_ige_decrypt(decrypted, encrypted_length, aes_key, aes_iv);
    tg_mtproto_msg_key(auth_key, decrypted, encrypted_length, x,
                       expected_msg_key);
    if (memcmp(expected_msg_key, payload + 8U, sizeof(expected_msg_key)) != 0) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }

    if (encrypted_length < 44UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memset(out, 0, sizeof(*out));
    out->server_salt_lo = tg_read_le32(decrypted);
    out->server_salt_hi = tg_read_le32(decrypted + 4U);
    memcpy(out->session_id, decrypted + 8U, sizeof(out->session_id));
    out->message_id_lo = tg_read_le32(decrypted + 16U);
    out->message_id_hi = tg_read_le32(decrypted + 20U);
    out->seq_no = tg_read_le32(decrypted + 24U);
    out->body_length = tg_read_le32(decrypted + 28U);
    if (out->body_length > sizeof(out->body) ||
        out->body_length > encrypted_length - 32UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    padding_length = encrypted_length - 32UL - out->body_length;
    if (padding_length < 12UL || padding_length > 1024UL) {
        return TG_MTPROTO_TL_INVALID_DATA;
    }
    memcpy(out->body, decrypted + 32U, (size_t)out->body_length);
    return TG_MTPROTO_TL_OK;
}

tg_mtproto_tl_status tg_mtproto_decrypt_encrypted_message(
    const unsigned char *payload,
    unsigned long payload_length,
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    tg_mtproto_encrypted_message *out)
{
    return tg_mtproto_decrypt_encrypted_message_x(payload, payload_length,
                                                  auth_key, 8U, out);
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_encrypted_self_test(void)
{
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    unsigned char session_id[8] = {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U
    };
    unsigned char padding[20];
    unsigned char body[12];
    unsigned char packet[256];
    unsigned long salt_hi;
    unsigned long salt_lo;
    unsigned long auth_hi;
    unsigned long auth_lo;
    static tg_mtproto_encrypted_message decoded;
    tg_mtproto_tl_writer writer;
    unsigned int i;

    for (i = 0U; i < sizeof(auth_key); ++i) {
        auth_key[i] = (unsigned char)i;
    }
    for (i = 0U; i < sizeof(padding); ++i) {
        padding[i] = (unsigned char)(0xa0U + i);
    }
    tg_store_le32(body, 0x7abe77ecUL);
    tg_store_le32(body + 4U, 0x55667788UL);
    tg_store_le32(body + 8U, 0x11223344UL);

    tg_mtproto_auth_key_id(auth_key, &auth_hi, &auth_lo);
    if (auth_hi != 0xc8df57a4UL || auth_lo != 0x6e58d132UL) {
        return 2;
    }
    tg_mtproto_initial_server_salt(auth_key, auth_key + 32U, &salt_hi,
                                   &salt_lo);
    if (salt_hi != 0x20202020UL || salt_lo != 0x20202020UL) {
        return 2;
    }

    tg_mtproto_tl_writer_init(&writer, packet, sizeof(packet));
    if (tg_mtproto_write_encrypted_message(
            &writer, auth_key, 0x99aabbccUL, 0xddeeff00UL, session_id,
            0x6777e5ebUL, 0x0005976cUL, 1UL, body, sizeof(body),
            padding, sizeof(padding)) != TG_MTPROTO_TL_OK ||
        writer.length == 0UL ||
        tg_mtproto_decrypt_encrypted_message_x(packet, writer.length,
                                               auth_key, 0U, &decoded) !=
            TG_MTPROTO_TL_OK ||
        decoded.server_salt_hi != 0x99aabbccUL ||
        decoded.server_salt_lo != 0xddeeff00UL ||
        memcmp(decoded.session_id, session_id, sizeof(session_id)) != 0 ||
        decoded.message_id_hi != 0x6777e5ebUL ||
        decoded.message_id_lo != 0x0005976cUL ||
        decoded.seq_no != 1UL ||
        decoded.body_length != sizeof(body) ||
        memcmp(decoded.body, body, sizeof(body)) != 0) {
        return 2;
    }

    /* Full-chunk round-trip: a getFile download chunk is RAW (uncompressed) and
       must survive encrypt->decrypt whole. This is the exact path that failed on
       OS4/MorphOS/AROS when the intermediate plaintext/decrypted buffers stayed
       at 12 KB while body[] was 72 KB: a full 64 KB chunk (8 KB on m68k) has to
       round-trip without tripping the sizeof() guards. Without the platform-gated
       TG_MTPROTO_ENCRYPTED_PACKET_MAX this returns 2 on the non-m68k lanes. */
    {
        /* Sized from the body cap itself, 8 KB under it: 64 KB on the 72 KB
           lanes (what a full download chunk is there), 32 KB on the 40 KB
           m68k body, 16 KB under the 24 KB low-memory profile, so the test
           follows whatever profile the build wears. */
        static unsigned char big[TG_MTPROTO_ENCRYPTED_BODY_MAX - 8192U];
        static unsigned char big_packet[TG_MTPROTO_ENCRYPTED_PACKET_MAX + 64U];
        static unsigned char big_pad[32];
        static tg_mtproto_encrypted_message big_decoded;
        tg_mtproto_tl_writer big_writer;
        unsigned long pad_len;
        unsigned int j;

        for (j = 0U; j < sizeof(big); ++j) {
            big[j] = (unsigned char)((j * 7U + 3U) & 0xffU);
        }
        pad_len = 12UL;
        while (((32UL + sizeof(big) + pad_len) % 16UL) != 0UL) {
            ++pad_len;
        }
        for (j = 0U; j < (unsigned int)pad_len; ++j) {
            big_pad[j] = (unsigned char)(0x50U + j);
        }
        tg_mtproto_tl_writer_init(&big_writer, big_packet, sizeof(big_packet));
        if (tg_mtproto_write_encrypted_message(
                &big_writer, auth_key, 0x99aabbccUL, 0xddeeff00UL, session_id,
                0x6777e5ebUL, 0x0005976dUL, 1UL, big, sizeof(big),
                big_pad, pad_len) != TG_MTPROTO_TL_OK ||
            tg_mtproto_decrypt_encrypted_message_x(
                big_packet, big_writer.length, auth_key, 0U, &big_decoded) !=
                TG_MTPROTO_TL_OK ||
            big_decoded.body_length != sizeof(big) ||
            memcmp(big_decoded.body, big, sizeof(big)) != 0) {
            return 2;
        }
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
