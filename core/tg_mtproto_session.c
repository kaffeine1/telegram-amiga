/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_file.h"
#include "tg_mtproto_encrypted.h"
#include "tg_mtproto_session.h"

void tg_mtproto_session_init(tg_mtproto_session *session)
{
    if (session != 0) {
        session->dc_id = 0;
        session->auth_key_id_hi = 0;
        session->auth_key_id_lo = 0;
        session->server_salt_hi = 0;
        session->server_salt_lo = 0;
        session->seq_no = 1;
        session->last_msg_id_hi = 0;
        session->last_msg_id_lo = 0;
        memset(session->session_id, 0, sizeof(session->session_id));
    }
}

void tg_mtproto_session_from_auth_key(
    tg_mtproto_session *session,
    unsigned long dc_id,
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    const unsigned char new_nonce[32],
    const unsigned char server_nonce[16],
    const unsigned char session_id[8])
{
    if (session == 0 || auth_key == 0 || new_nonce == 0 ||
        server_nonce == 0 || session_id == 0) {
        return;
    }
    tg_mtproto_session_init(session);
    session->dc_id = dc_id;
    tg_mtproto_auth_key_id(auth_key, &session->auth_key_id_hi,
                           &session->auth_key_id_lo);
    tg_mtproto_initial_server_salt(new_nonce, server_nonce,
                                   &session->server_salt_hi,
                                   &session->server_salt_lo);
    memcpy(session->session_id, session_id, sizeof(session->session_id));
}

static int tg_mtproto_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int tg_mtproto_parse_hex32(const char *text, unsigned long *value)
{
    unsigned long parsed;
    int digit;
    int i;

    if (text == 0 || value == 0) {
        return 1;
    }
    parsed = 0;
    for (i = 0; i < 8; ++i) {
        digit = tg_mtproto_hex_value(text[i]);
        if (digit < 0) {
            return 1;
        }
        parsed = ((parsed << 4) | (unsigned long)digit) & 0xffffffffUL;
    }
    if (text[8] != '\0' && text[8] != '\n' && text[8] != '\r') {
        return 1;
    }
    *value = parsed;
    return 0;
}

static int tg_mtproto_parse_hex_bytes(const char *text, unsigned char *bytes,
                                      unsigned long byte_count)
{
    unsigned long i;
    int hi;
    int lo;

    if (text == 0 || bytes == 0) {
        return 1;
    }
    for (i = 0; i < byte_count; ++i) {
        hi = tg_mtproto_hex_value(text[i * 2]);
        lo = tg_mtproto_hex_value(text[(i * 2) + 1]);
        if (hi < 0 || lo < 0) {
            return 1;
        }
        bytes[i] = (unsigned char)(((unsigned long)hi << 4) | (unsigned long)lo);
    }
    if (text[byte_count * 2] != '\0' &&
        text[byte_count * 2] != '\n' &&
        text[byte_count * 2] != '\r') {
        return 1;
    }
    return 0;
}

static void tg_mtproto_hex_bytes(char *dest, const unsigned char *bytes,
                                 unsigned long byte_count)
{
    static const char hex[] = "0123456789abcdef";
    unsigned long i;

    for (i = 0; i < byte_count; ++i) {
        dest[i * 2] = hex[(bytes[i] >> 4) & 0x0fU];
        dest[(i * 2) + 1] = hex[bytes[i] & 0x0fU];
    }
    dest[byte_count * 2] = '\0';
}

tg_mtproto_session_status tg_mtproto_session_save(const char *path,
                                                  const tg_mtproto_session *session)
{
    char text[512];
    char sid[17];
    int written;

    if (path == 0 || path[0] == '\0' || session == 0) {
        return TG_MTPROTO_SESSION_INVALID_ARGUMENT;
    }

    tg_mtproto_hex_bytes(sid, session->session_id, sizeof(session->session_id));
    written = sprintf(text,
                      "mtproto-session-v1\n"
                      "dc_id=%lu\n"
                      "auth_key_id_hi=%08lx\n"
                      "auth_key_id_lo=%08lx\n"
                      "server_salt_hi=%08lx\n"
                      "server_salt_lo=%08lx\n"
                      "seq_no=%lu\n"
                      "last_msg_id_hi=%08lx\n"
                      "last_msg_id_lo=%08lx\n"
                      "session_id=%s\n",
                      session->dc_id,
                      session->auth_key_id_hi & 0xffffffffUL,
                      session->auth_key_id_lo & 0xffffffffUL,
                      session->server_salt_hi & 0xffffffffUL,
                      session->server_salt_lo & 0xffffffffUL,
                      session->seq_no,
                      session->last_msg_id_hi & 0xffffffffUL,
                      session->last_msg_id_lo & 0xffffffffUL,
                      sid);
    if (written <= 0 || written >= (int)sizeof(text)) {
        return TG_MTPROTO_SESSION_BUFFER_TOO_SMALL;
    }
    if (tg_file_write_text(path, text, (unsigned long)written) != TG_FILE_OK) {
        return TG_MTPROTO_SESSION_FILE_ERROR;
    }
    return TG_MTPROTO_SESSION_OK;
}

static const char *tg_mtproto_line_value(const char *text, const char *key)
{
    unsigned long key_length;
    const char *line;

    key_length = (unsigned long)strlen(key);
    line = text;
    while (*line != '\0') {
        if (strncmp(line, key, (size_t)key_length) == 0 &&
            line[key_length] == '=') {
            return line + key_length + 1;
        }
        while (*line != '\0' && *line != '\n') {
            ++line;
        }
        if (*line == '\n') {
            ++line;
        }
    }
    return 0;
}

tg_mtproto_session_status tg_mtproto_session_load(const char *path,
                                                  tg_mtproto_session *session)
{
    char text[512];
    unsigned long text_length;
    const char *value;
    tg_mtproto_session loaded;

    if (path == 0 || path[0] == '\0' || session == 0) {
        return TG_MTPROTO_SESSION_INVALID_ARGUMENT;
    }
    if (tg_file_read_text(path, text, sizeof(text), &text_length) != TG_FILE_OK) {
        return TG_MTPROTO_SESSION_FILE_ERROR;
    }
    if (strncmp(text, "mtproto-session-v1\n",
                strlen("mtproto-session-v1\n")) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }

    tg_mtproto_session_init(&loaded);
    value = tg_mtproto_line_value(text, "dc_id");
    if (value == 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    loaded.dc_id = strtoul(value, 0, 10);

    value = tg_mtproto_line_value(text, "auth_key_id_hi");
    if (tg_mtproto_parse_hex32(value, &loaded.auth_key_id_hi) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "auth_key_id_lo");
    if (tg_mtproto_parse_hex32(value, &loaded.auth_key_id_lo) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "server_salt_hi");
    if (tg_mtproto_parse_hex32(value, &loaded.server_salt_hi) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "server_salt_lo");
    if (tg_mtproto_parse_hex32(value, &loaded.server_salt_lo) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "seq_no");
    if (value != 0) {
        loaded.seq_no = strtoul(value, 0, 10);
    }
    value = tg_mtproto_line_value(text, "last_msg_id_hi");
    if (value != 0 &&
        tg_mtproto_parse_hex32(value, &loaded.last_msg_id_hi) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "last_msg_id_lo");
    if (value != 0 &&
        tg_mtproto_parse_hex32(value, &loaded.last_msg_id_lo) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "session_id");
    if (tg_mtproto_parse_hex_bytes(value, loaded.session_id,
                                   sizeof(loaded.session_id)) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }

    *session = loaded;
    return TG_MTPROTO_SESSION_OK;
}

tg_mtproto_session_status tg_mtproto_session_save_authorization(
    const char *path,
    const tg_mtproto_session *session,
    const unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH],
    int secure_random_available)
{
    char text[1152];
    char sid[17];
    char key_hex[(TG_MTPROTO_AUTH_KEY_LENGTH * 2U) + 1U];
    int written;

    if (path == 0 || path[0] == '\0' || session == 0 || auth_key == 0) {
        return TG_MTPROTO_SESSION_INVALID_ARGUMENT;
    }
    if (!secure_random_available) {
        return TG_MTPROTO_SESSION_RNG_UNAVAILABLE;
    }

    tg_mtproto_hex_bytes(sid, session->session_id, sizeof(session->session_id));
    tg_mtproto_hex_bytes(key_hex, auth_key, TG_MTPROTO_AUTH_KEY_LENGTH);
    written = sprintf(text,
                      "mtproto-authorization-v1\n"
                      "dc_id=%lu\n"
                      "auth_key_id_hi=%08lx\n"
                      "auth_key_id_lo=%08lx\n"
                      "server_salt_hi=%08lx\n"
                      "server_salt_lo=%08lx\n"
                      "seq_no=%lu\n"
                      "last_msg_id_hi=%08lx\n"
                      "last_msg_id_lo=%08lx\n"
                      "session_id=%s\n"
                      "auth_key=%s\n",
                      session->dc_id,
                      session->auth_key_id_hi & 0xffffffffUL,
                      session->auth_key_id_lo & 0xffffffffUL,
                      session->server_salt_hi & 0xffffffffUL,
                      session->server_salt_lo & 0xffffffffUL,
                      session->seq_no,
                      session->last_msg_id_hi & 0xffffffffUL,
                      session->last_msg_id_lo & 0xffffffffUL,
                      sid,
                      key_hex);
    if (written <= 0 || written >= (int)sizeof(text)) {
        return TG_MTPROTO_SESSION_BUFFER_TOO_SMALL;
    }
    if (tg_file_write_text(path, text, (unsigned long)written) != TG_FILE_OK) {
        return TG_MTPROTO_SESSION_FILE_ERROR;
    }
    return TG_MTPROTO_SESSION_OK;
}

tg_mtproto_session_status tg_mtproto_session_load_authorization(
    const char *path,
    tg_mtproto_session *session,
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH])
{
    char text[1152];
    unsigned long text_length;
    const char *value;
    tg_mtproto_session loaded;
    unsigned char loaded_key[TG_MTPROTO_AUTH_KEY_LENGTH];

    if (path == 0 || path[0] == '\0' || session == 0 || auth_key == 0) {
        return TG_MTPROTO_SESSION_INVALID_ARGUMENT;
    }
    if (tg_file_read_text(path, text, sizeof(text), &text_length) != TG_FILE_OK) {
        return TG_MTPROTO_SESSION_FILE_ERROR;
    }
    if (strncmp(text, "mtproto-authorization-v1\n",
                strlen("mtproto-authorization-v1\n")) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }

    tg_mtproto_session_init(&loaded);
    value = tg_mtproto_line_value(text, "dc_id");
    if (value == 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    loaded.dc_id = strtoul(value, 0, 10);

    value = tg_mtproto_line_value(text, "auth_key_id_hi");
    if (tg_mtproto_parse_hex32(value, &loaded.auth_key_id_hi) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "auth_key_id_lo");
    if (tg_mtproto_parse_hex32(value, &loaded.auth_key_id_lo) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "server_salt_hi");
    if (tg_mtproto_parse_hex32(value, &loaded.server_salt_hi) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "server_salt_lo");
    if (tg_mtproto_parse_hex32(value, &loaded.server_salt_lo) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "seq_no");
    if (value != 0) {
        loaded.seq_no = strtoul(value, 0, 10);
    }
    value = tg_mtproto_line_value(text, "last_msg_id_hi");
    if (value != 0 &&
        tg_mtproto_parse_hex32(value, &loaded.last_msg_id_hi) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "last_msg_id_lo");
    if (value != 0 &&
        tg_mtproto_parse_hex32(value, &loaded.last_msg_id_lo) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "session_id");
    if (tg_mtproto_parse_hex_bytes(value, loaded.session_id,
                                   sizeof(loaded.session_id)) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }
    value = tg_mtproto_line_value(text, "auth_key");
    if (tg_mtproto_parse_hex_bytes(value, loaded_key,
                                   TG_MTPROTO_AUTH_KEY_LENGTH) != 0) {
        return TG_MTPROTO_SESSION_PARSE_ERROR;
    }

    *session = loaded;
    memcpy(auth_key, loaded_key, TG_MTPROTO_AUTH_KEY_LENGTH);
    return TG_MTPROTO_SESSION_OK;
}

const char *tg_mtproto_session_status_name(tg_mtproto_session_status status)
{
    switch (status) {
    case TG_MTPROTO_SESSION_OK:
        return "ok";
    case TG_MTPROTO_SESSION_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_MTPROTO_SESSION_FILE_ERROR:
        return "file-error";
    case TG_MTPROTO_SESSION_PARSE_ERROR:
        return "parse-error";
    case TG_MTPROTO_SESSION_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    case TG_MTPROTO_SESSION_RNG_UNAVAILABLE:
        return "rng-unavailable";
    default:
        return "unknown";
    }
}

#if !defined(TG_NO_SELFTEST)
int tg_mtproto_session_self_test(void)
{
    static const char path[] = "mtproto-session-self-test.tmp";
    tg_mtproto_session session;
    tg_mtproto_session loaded;
    unsigned char auth_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    unsigned char new_nonce[32];
    unsigned char server_nonce[16];
    unsigned char session_id[8];
    unsigned char loaded_key[TG_MTPROTO_AUTH_KEY_LENGTH];
    int i;

    /* The contract every saved file leans on: a write REPLACES, it never
       leaves a tail behind. A long file rewritten with a short one has to read
       back short. The reason it matters arrived from a Raspberry Pi, where
       rewriting an existing file on the FAT card silently kept zero bytes and
       a saved login vanished on every restart; tg_file_write_text now deletes
       before it creates. */
    {
        char probe[32];
        unsigned long probe_len;

        if (tg_file_write_text(path, "0123456789", 10UL) != TG_FILE_OK ||
            tg_file_write_text(path, "ab", 2UL) != TG_FILE_OK ||
            tg_file_read_text(path, probe, sizeof(probe), &probe_len)
                != TG_FILE_OK ||
            probe_len != 2UL || probe[0] != 'a' || probe[1] != 'b') {
            remove(path);
            return 2;
        }
        remove(path);
    }

    tg_mtproto_session_init(&session);
    session.dc_id = 2;
    session.auth_key_id_hi = 0x11223344UL;
    session.auth_key_id_lo = 0x55667788UL;
    session.server_salt_hi = 0x99aabbccUL;
    session.server_salt_lo = 0xddeeff00UL;
    session.seq_no = 7UL;
    session.last_msg_id_hi = 0x01020304UL;
    session.last_msg_id_lo = 0x05060708UL;
    for (i = 0; i < 8; ++i) {
        session.session_id[i] = (unsigned char)(0xa0 + i);
    }

    remove(path);
    if (tg_mtproto_session_save(path, &session) != TG_MTPROTO_SESSION_OK ||
        tg_mtproto_session_load(path, &loaded) != TG_MTPROTO_SESSION_OK) {
        remove(path);
        return 2;
    }
    remove(path);

    if (loaded.dc_id != session.dc_id ||
        loaded.auth_key_id_hi != session.auth_key_id_hi ||
        loaded.auth_key_id_lo != session.auth_key_id_lo ||
        loaded.server_salt_hi != session.server_salt_hi ||
        loaded.server_salt_lo != session.server_salt_lo ||
        loaded.seq_no != session.seq_no ||
        loaded.last_msg_id_hi != session.last_msg_id_hi ||
        loaded.last_msg_id_lo != session.last_msg_id_lo ||
        memcmp(loaded.session_id, session.session_id,
               sizeof(session.session_id)) != 0) {
        return 2;
    }

    for (i = 0; i < (int)sizeof(auth_key); ++i) {
        auth_key[i] = (unsigned char)i;
    }
    memset(new_nonce, 0x11, sizeof(new_nonce));
    memset(server_nonce, 0x22, sizeof(server_nonce));
    for (i = 0; i < (int)sizeof(session_id); ++i) {
        session_id[i] = (unsigned char)(0xb0 + i);
    }
    tg_mtproto_session_from_auth_key(&loaded, 4UL, auth_key, new_nonce,
                                     server_nonce, session_id);
    if (loaded.dc_id != 4UL ||
        loaded.auth_key_id_hi != 0xc8df57a4UL ||
        loaded.auth_key_id_lo != 0x6e58d132UL ||
        loaded.server_salt_hi != 0x33333333UL ||
        loaded.server_salt_lo != 0x33333333UL ||
        memcmp(loaded.session_id, session_id, sizeof(session_id)) != 0) {
        return 2;
    }

    remove(path);
    if (tg_mtproto_session_save_authorization(path, &loaded, auth_key, 0) !=
            TG_MTPROTO_SESSION_RNG_UNAVAILABLE ||
        tg_mtproto_session_load_authorization(path, &session, loaded_key) !=
            TG_MTPROTO_SESSION_FILE_ERROR) {
        remove(path);
        return 2;
    }
    if (tg_mtproto_session_save_authorization(path, &loaded, auth_key, 1) !=
            TG_MTPROTO_SESSION_OK ||
        tg_mtproto_session_load_authorization(path, &session, loaded_key) !=
            TG_MTPROTO_SESSION_OK) {
        remove(path);
        return 2;
    }
    remove(path);
    if (session.dc_id != loaded.dc_id ||
        session.auth_key_id_hi != loaded.auth_key_id_hi ||
        session.auth_key_id_lo != loaded.auth_key_id_lo ||
        session.server_salt_hi != loaded.server_salt_hi ||
        session.server_salt_lo != loaded.server_salt_lo ||
        session.seq_no != loaded.seq_no ||
        session.last_msg_id_hi != loaded.last_msg_id_hi ||
        session.last_msg_id_lo != loaded.last_msg_id_lo ||
        memcmp(session.session_id, loaded.session_id,
               sizeof(session.session_id)) != 0 ||
        memcmp(loaded_key, auth_key, sizeof(auth_key)) != 0) {
        return 2;
    }

    return 0;
}
#endif /* !TG_NO_SELFTEST */
