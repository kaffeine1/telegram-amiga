/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TG_HTTPS_H
#define TG_HTTPS_H

#include "tg_net.h"
#include "tg_tls.h"

/**
 * HTTPS transfer result.
 *
 * TG_HTTPS_TLS_ERROR means tls_status contains the TLS reason. If tls_status is
 * TG_TLS_NET_ERROR, net_status contains the lower-level TCP reason.
 */
typedef enum tg_https_status {
    TG_HTTPS_OK = 0,
    TG_HTTPS_INVALID_ARGUMENT = 1,
    TG_HTTPS_REQUEST_TOO_LARGE = 2,
    TG_HTTPS_RESPONSE_TOO_LARGE = 3,
    TG_HTTPS_TLS_ERROR = 4
} tg_https_status;

/**
 * Performs a minimal HTTP/1.0 GET over TLS.
 *
 * response_buffer is caller-owned and receives a NUL-terminated full HTTP
 * response. response_length excludes the final NUL. Certificate validation is
 * platform-dependent and currently not complete on all targets.
 */
tg_https_status tg_https_get(const char *host, const char *port, const char *path,
                             char *response_buffer, unsigned long response_buffer_size,
                             unsigned long *response_length, tg_tls_status *tls_status,
                             tg_net_status *net_status, char *error_buffer,
                             unsigned long error_buffer_size);

/**
 * Performs a minimal HTTP/1.0 POST over TLS.
 *
 * content_type and body are copied into the request before sending.
 * response_buffer is caller-owned and receives a NUL-terminated full HTTP
 * response. Certificate validation is platform-dependent and currently not
 * complete on all targets.
 */
tg_https_status tg_https_post(const char *host, const char *port, const char *path,
                              const char *content_type, const char *body,
                              unsigned long body_length, char *response_buffer,
                              unsigned long response_buffer_size,
                              unsigned long *response_length, tg_tls_status *tls_status,
                              tg_net_status *net_status, char *error_buffer,
                              unsigned long error_buffer_size);

/**
 * Returns a static string for status. The caller must not free it.
 */
const char *tg_https_status_name(tg_https_status status);

#endif
