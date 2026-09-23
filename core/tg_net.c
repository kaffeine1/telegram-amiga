/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include "tg_net.h"
#include "tg_platform.h"
#ifdef TG_DIAG_TRACE
/* Field diagnosis (68000 lane): the gap between "session loaded" and the
   first history line covers DNS, connect, the first send and the first
   reply. Naming each one turns "it crashes with the network on" into a
   single line that says which call never came back. */
#include <stdio.h>
#include "tg_gui_session.h"
static void tg_net_diag(const char *what, unsigned long n)
{
    char line[80];

    sprintf(line, "net: %.40s %lu", what, n);
    tg_gui_log(line);
}
#define TG_NET_DIAG(what, n) tg_net_diag((what), (unsigned long)(n))
#else
#define TG_NET_DIAG(what, n) ((void)0)
#endif

#if defined(TG_DIAG_XFER)
#include <stdio.h>
#include <sys/time.h>
#include "tg_gui_session.h"
static unsigned long tg_xfer_sum[TG_XFER_COUNT];
static unsigned long tg_xfer_began[TG_XFER_COUNT];
static int tg_xfer_armed[TG_XFER_COUNT];
static unsigned long tg_xfer_mark;

/* Microseconds, wrapping every 71 minutes: only differences are used. */
static unsigned long tg_xfer_now(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, 0) != 0) {
        return 0UL;
    }
    return (unsigned long)tv.tv_sec * 1000000UL + (unsigned long)tv.tv_usec;
}

unsigned long tg_net_xfer_clock_ms(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, 0) != 0) {
        return 0UL;
    }
    return (unsigned long)tv.tv_sec * 1000UL +
           (unsigned long)tv.tv_usec / 1000UL;
}

void tg_net_xfer_reset(void)
{
    int i;

    for (i = 0; i < TG_XFER_COUNT; ++i) {
        tg_xfer_sum[i] = 0UL;
        tg_xfer_armed[i] = 0;
    }
    tg_xfer_mark = tg_xfer_now();
}

void tg_net_xfer_add(int slot, unsigned long value)
{
    if (slot >= 0 && slot < TG_XFER_COUNT) {
        tg_xfer_sum[slot] += value;
    }
}

void tg_net_xfer_start(int slot)
{
    if (slot >= 0 && slot < TG_XFER_COUNT) {
        tg_xfer_began[slot] = tg_xfer_now();
        tg_xfer_armed[slot] = 1;
    }
}

void tg_net_xfer_stop(int slot)
{
    if (slot >= 0 && slot < TG_XFER_COUNT && tg_xfer_armed[slot]) {
        tg_xfer_sum[slot] += tg_xfer_now() - tg_xfer_began[slot];
        tg_xfer_armed[slot] = 0;
    }
}

/* "12.3" from microseconds, into out (at least 16 bytes). */
static const char *tg_xfer_ms(char *out, unsigned long us)
{
    sprintf(out, "%lu.%lu", us / 1000UL, (us % 1000UL) / 100UL);
    return out;
}

void tg_net_xfer_report(const char *what, unsigned long offset)
{
    char line[320];
    char a[16], b[16], c[16], d[16], e[16], f[16], g[16], h[16], k[16], m[16];
    char o[16];
    unsigned long now = tg_xfer_now();
    unsigned long wall = now - tg_xfer_mark;
    unsigned long known = tg_xfer_sum[TG_XFER_WAIT_US] +
                          tg_xfer_sum[TG_XFER_STREAM_US] +
                          tg_xfer_sum[TG_XFER_SEND_US] +
                          tg_xfer_sum[TG_XFER_ENC_US] +
                          tg_xfer_sum[TG_XFER_DEC_US] +
                          tg_xfer_sum[TG_XFER_WRITE_US] +
                          tg_xfer_sum[TG_XFER_LOG_US] +
                          tg_xfer_sum[TG_XFER_LOOP_US];
    unsigned long other = wall > known ? wall - known : 0UL;
    int i;

    sprintf(line,
            "xfer %.8s off=%lu wall=%s wait=%s stream=%s recv=%s n=%lu b=%lu "
            "send=%s enc=%s dec=%s write=%s log=%s loop=%s pk=%lu other=%s",
            what, offset, tg_xfer_ms(a, wall),
            tg_xfer_ms(b, tg_xfer_sum[TG_XFER_WAIT_US]),
            tg_xfer_ms(c, tg_xfer_sum[TG_XFER_STREAM_US]),
            tg_xfer_ms(d, tg_xfer_sum[TG_XFER_RECV_US]),
            tg_xfer_sum[TG_XFER_RECV_CALLS], tg_xfer_sum[TG_XFER_RECV_BYTES],
            tg_xfer_ms(e, tg_xfer_sum[TG_XFER_SEND_US]),
            tg_xfer_ms(f, tg_xfer_sum[TG_XFER_ENC_US]),
            tg_xfer_ms(g, tg_xfer_sum[TG_XFER_DEC_US]),
            tg_xfer_ms(h, tg_xfer_sum[TG_XFER_WRITE_US]),
            tg_xfer_ms(k, tg_xfer_sum[TG_XFER_LOG_US]),
            tg_xfer_ms(m, tg_xfer_sum[TG_XFER_LOOP_US]),
            tg_xfer_sum[TG_XFER_PACKETS], tg_xfer_ms(o, other));
    for (i = 0; i < TG_XFER_COUNT; ++i) {
        tg_xfer_sum[i] = 0UL;
    }
    tg_xfer_mark = now;
    tg_gui_log(line); /* its own cost lands in the next line's log= */
}
#endif

static unsigned long tg_connect_timeout_seconds = 0;

void tg_net_connection_init(tg_net_connection *connection)
{
    if (connection != 0) {
        connection->platform_handle = -1;
        connection->is_open = 0;
    }
}

void tg_net_set_connect_timeout_seconds(unsigned long seconds)
{
    if (seconds > 3600UL) {
        seconds = 3600UL;
    }
    tg_connect_timeout_seconds = seconds;
}

unsigned long tg_net_connect_timeout_seconds(void)
{
    return tg_connect_timeout_seconds;
}

tg_net_status tg_net_connect(tg_net_connection *connection, const char *host, const char *port,
                             char *error_buffer, unsigned long error_buffer_size)
{
    if (connection == 0 || host == 0 || port == 0 || host[0] == '\0' || port[0] == '\0') {
        return TG_NET_INVALID_ARGUMENT;
    }

    tg_net_connection_init(connection);
    TG_NET_DIAG("connect begin", 0);
    {
        tg_net_status st;

        st = tg_platform_tcp_connect(connection, host, port, error_buffer,
                                     error_buffer_size);
        TG_NET_DIAG("connect done rc", (unsigned long)st);
        return st;
    }
}

tg_net_status tg_net_send(tg_net_connection *connection, const void *data,
                          unsigned long byte_count, unsigned long *bytes_sent,
                          char *error_buffer, unsigned long error_buffer_size)
{
    if (bytes_sent != 0) {
        *bytes_sent = 0;
    }
    if (connection == 0 || data == 0 || byte_count == 0) {
        return TG_NET_INVALID_ARGUMENT;
    }
    if (!connection->is_open) {
        return TG_NET_CLOSED;
    }

    TG_NET_DIAG("send begin", byte_count);
    {
        tg_net_status st;

        TG_XFER_START(TG_XFER_SEND_US);
        st = tg_platform_tcp_send(connection, data, byte_count, bytes_sent,
                                  error_buffer, error_buffer_size);
        TG_XFER_STOP(TG_XFER_SEND_US);
        TG_NET_DIAG("send done rc", (unsigned long)st);
        return st;
    }
}

tg_net_status tg_net_recv(tg_net_connection *connection, void *buffer,
                          unsigned long buffer_size, unsigned long *bytes_received,
                          char *error_buffer, unsigned long error_buffer_size)
{
    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (connection == 0 || buffer == 0 || buffer_size == 0) {
        return TG_NET_INVALID_ARGUMENT;
    }
    if (!connection->is_open) {
        return TG_NET_CLOSED;
    }

    TG_NET_DIAG("recv begin", buffer_size);
    {
        tg_net_status st;

        TG_XFER_START(TG_XFER_RECV_US);
        st = tg_platform_tcp_recv(connection, buffer, buffer_size,
                                  bytes_received, error_buffer,
                                  error_buffer_size);
        TG_XFER_STOP(TG_XFER_RECV_US);
        TG_XFER_ADD(TG_XFER_RECV_CALLS, 1);
        TG_XFER_ADD(TG_XFER_RECV_BYTES,
                    bytes_received != 0 ? *bytes_received : 0UL);
        TG_NET_DIAG("recv done rc", (unsigned long)st);
        return st;
    }
}

int tg_net_poll_readable(tg_net_connection *connection,
                         char *error_buffer, unsigned long error_buffer_size)
{
    if (error_buffer != 0 && error_buffer_size > 0UL) {
        error_buffer[0] = '\0';
    }
    if (connection == 0 || !connection->is_open) {
        return -1;
    }
    return tg_platform_tcp_poll_readable(connection, error_buffer,
                                         error_buffer_size);
}

void tg_net_close(tg_net_connection *connection)
{
    if (connection != 0 && connection->is_open) {
        tg_platform_tcp_close(connection);
    }
    tg_net_connection_init(connection);
}

tg_net_status tg_net_tcp_probe(const char *host, const char *port,
                               char *error_buffer, unsigned long error_buffer_size)
{
    tg_net_connection connection;
    tg_net_status status;

    if (host == 0 || port == 0 || host[0] == '\0' || port[0] == '\0') {
        return TG_NET_INVALID_ARGUMENT;
    }

    status = tg_net_connect(&connection, host, port, error_buffer, error_buffer_size);
    if (status == TG_NET_OK) {
        tg_net_close(&connection);
    }
    return status;
}

const char *tg_net_status_name(tg_net_status status)
{
    switch (status) {
    case TG_NET_OK:
        return "ok";
    case TG_NET_INVALID_ARGUMENT:
        return "invalid-argument";
    case TG_NET_RESOLVE_FAILED:
        return "resolve-failed";
    case TG_NET_CONNECT_FAILED:
        return "connect-failed";
    case TG_NET_SEND_FAILED:
        return "send-failed";
    case TG_NET_RECV_FAILED:
        return "recv-failed";
    case TG_NET_CLOSED:
        return "closed";
    case TG_NET_UNSUPPORTED:
        return "unsupported";
    case TG_NET_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}
