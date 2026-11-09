/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tg_platform.h"
#include "tg_tls.h"

static int tg_tls_verify_certificates = 0;
static const char *tg_tls_verify_ca_file = 0;
static const char *tg_tls_verify_ca_path = 0;

void tg_tls_connection_init(tg_tls_connection *connection)
{
    if (connection != 0) {
        tg_net_connection_init(&connection->tcp);
        connection->platform_context = 0;
        connection->platform_session = 0;
        connection->is_open = 0;
    }
}

void tg_tls_set_certificate_validation(int enabled,
                                       const char *ca_file,
                                       const char *ca_path)
{
    tg_tls_verify_certificates = enabled ? 1 : 0;
    tg_tls_verify_ca_file = ca_file;
    tg_tls_verify_ca_path = ca_path;
}

int tg_tls_certificate_validation_enabled(void)
{
    return tg_tls_verify_certificates;
}

const char *tg_tls_certificate_ca_file(void)
{
    return tg_tls_verify_ca_file;
}

const char *tg_tls_certificate_ca_path(void)
{
    return tg_tls_verify_ca_path;
}

tg_tls_status tg_tls_connect(tg_tls_connection *connection, const char *host, const char *port,
                             tg_net_status *net_status, char *error_buffer,
                             unsigned long error_buffer_size)
{
    if (connection == 0 || host == 0 || port == 0 || host[0] == '\0' || port[0] == '\0') {
        return TG_TLS_INVALID_ARGUMENT;
    }

    tg_tls_connection_init(connection);
    return tg_platform_tls_connect(connection, host, port, net_status,
                                   error_buffer, error_buffer_size);
}

tg_tls_status tg_tls_send(tg_tls_connection *connection, const void *data,
                          unsigned long byte_count, unsigned long *bytes_sent,
                          char *error_buffer, unsigned long error_buffer_size)
{
    if (bytes_sent != 0) {
        *bytes_sent = 0;
    }
    if (connection == 0 || data == 0 || byte_count == 0) {
        return TG_TLS_INVALID_ARGUMENT;
    }
    if (!connection->is_open) {
        return TG_TLS_CLOSED;
    }

    return tg_platform_tls_send(connection, data, byte_count, bytes_sent,
                                error_buffer, error_buffer_size);
}

tg_tls_status tg_tls_recv(tg_tls_connection *connection, void *buffer,
                          unsigned long buffer_size, unsigned long *bytes_received,
                          char *error_buffer, unsigned long error_buffer_size)
{
    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (connection == 0 || buffer == 0 || buffer_size == 0) {
        return TG_TLS_INVALID_ARGUMENT;
    }
    if (!connection->is_open) {
        return TG_TLS_CLOSED;
    }

    return tg_platform_tls_recv(connection, buffer, buffer_size, bytes_received,
                                error_buffer, error_buffer_size);
}

void tg_tls_close(tg_tls_connection *connection)
{
    if (connection != 0 && connection->is_open) {
        tg_platform_tls_close(connection);
    }
    tg_tls_connection_init(connection);
}

const char *tg_tls_status_name(tg_tls_status status)
{
    switch (status) {
    case TG_TLS_OK:
        return "ok";
    case TG_TLS_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_TLS_NET_ERROR:
        return "net-error";
    case TG_TLS_HANDSHAKE_FAILED:
        return "handshake-failed";
    case TG_TLS_SEND_FAILED:
        return "send-failed";
    case TG_TLS_RECV_FAILED:
        return "recv-failed";
    case TG_TLS_CLOSED:
        return "closed";
    case TG_TLS_UNSUPPORTED:
        return "unsupported";
    case TG_TLS_VERIFY_FAILED:
        return "verify-failed";
    default:
        return "unknown";
    }
}
