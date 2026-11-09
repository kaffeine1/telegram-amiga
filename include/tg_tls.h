/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_TLS_H
#define TG_TLS_H

#include "tg_net.h"

/**
 * TLS operation result.
 *
 * TG_TLS_NET_ERROR means the underlying TCP layer failed and net_status contains
 * the TCP reason. TG_TLS_UNSUPPORTED is returned by platforms without TLS.
 */
typedef enum tg_tls_status {
    TG_TLS_OK = 0,
    TG_TLS_INVALID_ARGUMENT = 1,
    TG_TLS_NET_ERROR = 2,
    TG_TLS_HANDSHAKE_FAILED = 3,
    TG_TLS_SEND_FAILED = 4,
    TG_TLS_RECV_FAILED = 5,
    TG_TLS_CLOSED = 6,
    TG_TLS_UNSUPPORTED = 7,
    TG_TLS_VERIFY_FAILED = 8
} tg_tls_status;

/**
 * Portable TLS connection.
 *
 * platform_context and platform_session are owned by the TLS backend while the
 * connection is open. Call tg_tls_close() to release them.
 */
typedef struct tg_tls_connection {
    tg_net_connection tcp;
    void *platform_context;
    void *platform_session;
    int is_open;
} tg_tls_connection;

/**
 * Resets a TLS connection object to a closed state.
 */
void tg_tls_connection_init(tg_tls_connection *connection);

/**
 * Sets process-wide certificate verification options for later TLS connects.
 *
 * The strings are borrowed and must remain valid for the process lifetime or
 * until the next call. Passing enabled=0 disables certificate validation.
 */
void tg_tls_set_certificate_validation(int enabled,
                                       const char *ca_file,
                                       const char *ca_path);

/**
 * Returns non-zero when certificate validation is requested.
 */
int tg_tls_certificate_validation_enabled(void);

/**
 * Returns the configured CA file/path, or NULL when not configured.
 */
const char *tg_tls_certificate_ca_file(void);
const char *tg_tls_certificate_ca_path(void);

/**
 * Opens a TCP connection and performs the TLS handshake.
 *
 * error_buffer is caller-owned and optional. net_status is optional and is set
 * when the failure comes from the TCP layer.
 */
tg_tls_status tg_tls_connect(tg_tls_connection *connection, const char *host, const char *port,
                             tg_net_status *net_status, char *error_buffer,
                             unsigned long error_buffer_size);

/**
 * Sends encrypted data. Short writes are possible; bytes_sent is caller-owned.
 */
tg_tls_status tg_tls_send(tg_tls_connection *connection, const void *data,
                          unsigned long byte_count, unsigned long *bytes_sent,
                          char *error_buffer, unsigned long error_buffer_size);

/**
 * Receives encrypted data into caller-owned buffer.
 */
tg_tls_status tg_tls_recv(tg_tls_connection *connection, void *buffer,
                          unsigned long buffer_size, unsigned long *bytes_received,
                          char *error_buffer, unsigned long error_buffer_size);

/**
 * Shuts down TLS and closes the underlying TCP connection.
 */
void tg_tls_close(tg_tls_connection *connection);

/**
 * Returns a static string for status. The caller must not free it.
 */
const char *tg_tls_status_name(tg_tls_status status);

#endif
