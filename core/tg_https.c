/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "tg_http.h"
#include "tg_https.h"

static tg_https_status tg_https_from_http_status(tg_http_status status)
{
    if (status == TG_HTTP_OK) {
        return TG_HTTPS_OK;
    }
    if (status == TG_HTTP_REQUEST_TOO_LARGE) {
        return TG_HTTPS_REQUEST_TOO_LARGE;
    }
    return TG_HTTPS_INVALID_ARGUMENT;
}

static tg_https_status tg_https_send_request(const char *host, const char *port,
                                             const char *request,
                                             unsigned long request_length,
                                             char *response_buffer,
                                             unsigned long response_buffer_size,
                                             unsigned long *response_length,
                                             tg_tls_status *tls_status,
                                             tg_net_status *net_status,
                                             char *error_buffer,
                                             unsigned long error_buffer_size)
{
    tg_tls_connection connection;
    tg_tls_status local_tls_status;
    unsigned long total_sent;
    unsigned long bytes_done;
    unsigned long total_received;

    local_tls_status = tg_tls_connect(&connection, host, port, net_status,
                                      error_buffer, error_buffer_size);
    if (local_tls_status != TG_TLS_OK) {
        if (tls_status != 0) {
            *tls_status = local_tls_status;
        }
        return TG_HTTPS_TLS_ERROR;
    }

    total_sent = 0;
    while (total_sent < request_length) {
        local_tls_status = tg_tls_send(&connection, request + total_sent,
                                       request_length - total_sent, &bytes_done,
                                       error_buffer, error_buffer_size);
        if (local_tls_status != TG_TLS_OK || bytes_done == 0) {
            tg_tls_close(&connection);
            if (tls_status != 0) {
                *tls_status = local_tls_status;
            }
            return TG_HTTPS_TLS_ERROR;
        }
        total_sent += bytes_done;
    }

    total_received = 0;
    while (total_received < response_buffer_size - 1) {
        local_tls_status = tg_tls_recv(&connection, response_buffer + total_received,
                                       response_buffer_size - 1 - total_received, &bytes_done,
                                       error_buffer, error_buffer_size);
        if (local_tls_status == TG_TLS_CLOSED) {
            break;
        }
        if (local_tls_status != TG_TLS_OK) {
            if (total_received > 0) {
                break;
            }
            tg_tls_close(&connection);
            response_buffer[total_received] = '\0';
            *response_length = total_received;
            if (tls_status != 0) {
                *tls_status = local_tls_status;
            }
            return TG_HTTPS_TLS_ERROR;
        }
        if (bytes_done == 0) {
            break;
        }
        total_received += bytes_done;
    }

    tg_tls_close(&connection);
    response_buffer[total_received] = '\0';
    *response_length = total_received;

    if (total_received >= response_buffer_size - 1) {
        return TG_HTTPS_RESPONSE_TOO_LARGE;
    }

    return TG_HTTPS_OK;
}

tg_https_status tg_https_get(const char *host, const char *port, const char *path,
                             char *response_buffer, unsigned long response_buffer_size,
                             unsigned long *response_length, tg_tls_status *tls_status,
                             tg_net_status *net_status, char *error_buffer,
                             unsigned long error_buffer_size)
{
    tg_http_status http_status;
    tg_https_status https_status;
    char request[512];
    unsigned long request_length;

    if (response_length != 0) {
        *response_length = 0;
    }
    if (tls_status != 0) {
        *tls_status = TG_TLS_OK;
    }
    if (net_status != 0) {
        *net_status = TG_NET_OK;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    if (host == 0 || port == 0 || path == 0 || response_buffer == 0 ||
        response_buffer_size < 2 || response_length == 0) {
        return TG_HTTPS_INVALID_ARGUMENT;
    }

    http_status = tg_http_build_get_request(host, path, request,
                                            sizeof(request), &request_length);
    https_status = tg_https_from_http_status(http_status);
    if (https_status != TG_HTTPS_OK) {
        return https_status;
    }

    return tg_https_send_request(host, port, request, request_length,
                                 response_buffer, response_buffer_size,
                                 response_length, tls_status, net_status,
                                 error_buffer, error_buffer_size);
}

tg_https_status tg_https_post(const char *host, const char *port, const char *path,
                              const char *content_type, const char *body,
                              unsigned long body_length, char *response_buffer,
                              unsigned long response_buffer_size,
                              unsigned long *response_length, tg_tls_status *tls_status,
                              tg_net_status *net_status, char *error_buffer,
                              unsigned long error_buffer_size)
{
    tg_http_status http_status;
    tg_https_status https_status;
    char request[2048];
    unsigned long request_length;

    if (response_length != 0) {
        *response_length = 0;
    }
    if (tls_status != 0) {
        *tls_status = TG_TLS_OK;
    }
    if (net_status != 0) {
        *net_status = TG_NET_OK;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    if (host == 0 || port == 0 || path == 0 || content_type == 0 ||
        response_buffer == 0 || response_buffer_size < 2 ||
        response_length == 0 || (body == 0 && body_length > 0)) {
        return TG_HTTPS_INVALID_ARGUMENT;
    }

    http_status = tg_http_build_post_request(host, path, content_type,
                                             body, body_length, request,
                                             sizeof(request), &request_length);
    https_status = tg_https_from_http_status(http_status);
    if (https_status != TG_HTTPS_OK) {
        return https_status;
    }

    return tg_https_send_request(host, port, request, request_length,
                                 response_buffer, response_buffer_size,
                                 response_length, tls_status, net_status,
                                 error_buffer, error_buffer_size);
}

const char *tg_https_status_name(tg_https_status status)
{
    switch (status) {
    case TG_HTTPS_OK:
        return "ok";
    case TG_HTTPS_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_HTTPS_REQUEST_TOO_LARGE:
        return "request-too-large";
    case TG_HTTPS_RESPONSE_TOO_LARGE:
        return "response-too-large";
    case TG_HTTPS_TLS_ERROR:
        return "tls-error";
    default:
        return "unknown";
    }
}
