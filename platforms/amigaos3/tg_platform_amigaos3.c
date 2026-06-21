/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#if !defined(__amigaos3__)
#include <fcntl.h>
#endif

#ifndef TG_AMIGAOS3_ENABLE_AMISSL
#define TG_AMIGAOS3_ENABLE_AMISSL 0
#endif

/* When built without ixemul (clib2/libnix), the C library provides no BSD
   socket calls, so we must reach bsdsocket.library directly through the
   AmigaOS SDK inlines (proto/socket.h). */
#ifndef TG_AMIGAOS3_NOIXEMUL
#define TG_AMIGAOS3_NOIXEMUL 0
#endif

/* Both AmiSSL and the no-ixemul build talk to bsdsocket.library directly
   (SocketBase + proto/socket.h inlines), so they share the socket open/close/
   WaitSelect path. The plain ixemul build uses the C library's sockets. */
#define TG_AMIGAOS3_BSDSOCKET_DIRECT \
    (TG_AMIGAOS3_ENABLE_AMISSL || TG_AMIGAOS3_NOIXEMUL)

#if defined(__amigaos3__)
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#endif

#if defined(__amigaos3__)
#include <time.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/timer.h>
#include <devices/timer.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#if defined(__amigaos3__) && (TG_AMIGAOS3_ENABLE_AMISSL || TG_AMIGAOS3_NOIXEMUL)
#include <proto/exec.h>
/* Some BSD headers want a few guard names that ixemul (and a few C libraries)
   omit; define them so proto/socket.h pulls cleanly. */
#ifndef SYS_MBUF_H
#define SYS_MBUF_H
#endif
#ifndef _SYS_MBUF_H_
#define _SYS_MBUF_H_
#endif
#ifndef NET_ROUTE_H
#define NET_ROUTE_H
#endif
#ifndef _NET_ROUTE_H_
#define _NET_ROUTE_H_
#endif
/* socket()/connect()/recv()/send()/gethostbyname()/CloseSocket()/WaitSelect()
   resolve to bsdsocket.library inlines through SocketBase -- required without
   ixemul, and also used by the AmiSSL path. */
#include <proto/socket.h>
#endif

#if defined(__amigaos3__) && TG_AMIGAOS3_ENABLE_AMISSL
#include <proto/amissl.h>
#include <proto/amisslmaster.h>
#include <amissl/amissl.h>
#include <libraries/amissl.h>
#include <libraries/amisslmaster.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#endif

#include "tg_platform.h"
#include "tg_mtproto_crypto.h"

#if defined(__amigaos3__) && TG_AMIGAOS3_ENABLE_AMISSL
#ifndef TG_AMIGAOS3_AMISSL_API_VERSION
#define TG_AMIGAOS3_AMISSL_API_VERSION AMISSL_CURRENT_VERSION
#endif

struct Library *AmiSSLMasterBase = 0;
struct Library *AmiSSLBase = 0;
struct Library *AmiSSLExtBase = 0;
struct Library *SocketBase = 0;

static int tg_amigaos3_amissl_initialized = 0;

const char stack_size[] = "$STACK:65536";

static int tg_amigaos3_amissl_init(char *error_buffer,
                                   unsigned long error_buffer_size);
#endif

#if defined(__amigaos3__) && TG_AMIGAOS3_NOIXEMUL && !TG_AMIGAOS3_ENABLE_AMISSL
/* bsdsocket.library base used by the proto/socket.h inlines (no ixemul to
   provide BSD sockets). Opened in tg_amigaos3_socket_open(). */
struct Library *SocketBase = 0;
/* Embed a big stack cookie for the modexp / PBKDF2 crypto when launched
   outside a shell (e.g. by double-clicking the icon). */
const char stack_size[] = "$STACK:262144";
#endif

const char *tg_platform_name(void)
{
    return "AmigaOS 3.x";
}

const char *tg_platform_default_data_dir(void)
{
    return "PROGDIR:";
}

void tg_platform_log(const char *level, const char *message)
{
    printf("[amigaos3:%s] %s\n", level, message);
}

void tg_platform_debug(const char *message)
{
    (void)message; /* no dedicated kernel-debug channel used here */
}

void tg_platform_sleep_seconds(unsigned long seconds)
{
#if defined(__amigaos3__)
    unsigned long ticks;

    if (seconds > 0) {
        if (seconds > (2147483647UL / 50UL)) {
            ticks = 2147483647UL;
        } else {
            ticks = seconds * 50UL;
        }
        Delay((LONG)ticks);
    }
#else
    if (seconds > 0) {
        sleep((unsigned int)seconds);
    }
#endif
}

int tg_platform_stdin_readable(unsigned long timeout_seconds)
{
#if defined(__amigaos3__)
    unsigned long long timeout_microseconds;

    timeout_microseconds = (unsigned long long)timeout_seconds * 1000000ULL;
    if (timeout_microseconds > 2147000000ULL) {
        timeout_microseconds = 2147000000ULL;
    }
    return WaitForChar(Input(), (long)timeout_microseconds) != 0;
#else
    (void)timeout_seconds;
    return 0;
#endif
}

/*
 * Keystroke-timing entropy: every byte read from the console folds its
 * arrival time into a small ring at O(1) cost (no hashing on the input
 * path); the DRBG absorbs the ring on every generate. Human typing right
 * before the MTProto auth-key DH is entropy the E-Clock jitter loop alone
 * cannot provide on a quiet machine.
 */
#if defined(__amigaos3__)
static unsigned long tg_os3_timebase(void);

static unsigned long tg_os3_key_ring[16];
static unsigned long tg_os3_key_ring_pos = 0;

static void tg_os3_note_input_event(int ch)
{
    unsigned long v = tg_os3_timebase() ^
                      ((unsigned long)(unsigned char)ch << 24) ^
                      (tg_os3_key_ring_pos * 2654435761UL);
    tg_os3_key_ring[tg_os3_key_ring_pos & 15UL] ^=
        (v << (tg_os3_key_ring_pos & 7UL)) ^ (v >> 5);
    ++tg_os3_key_ring_pos;
}
#endif

int tg_platform_stdin_read_char(unsigned long timeout_seconds, char *out_char)
{
#if defined(__amigaos3__)
    unsigned long long timeout_microseconds;
    char ch;
    LONG got;

    if (out_char == 0) {
        return -1;
    }
    timeout_microseconds = (unsigned long long)timeout_seconds * 1000000ULL;
    if (timeout_microseconds > 2147000000ULL) {
        timeout_microseconds = 2147000000ULL;
    }
    if (WaitForChar(Input(), (long)timeout_microseconds) == 0) {
        return 0;
    }
    got = Read(Input(), &ch, 1);
    if (got <= 0) {
        return -1;
    }
    tg_os3_note_input_event((int)ch);
    *out_char = ch;
    return 1;
#else
    (void)timeout_seconds;
    (void)out_char;
    return 0;
#endif
}

int tg_platform_stdin_read_hidden_line(char *out, unsigned long out_size)
{
#if defined(__amigaos3__)
    unsigned long pos;
    char ch;
    LONG got;

    if (out == 0 || out_size == 0UL) {
        return -1;
    }
    out[0] = '\0';
    pos = 0UL;
    SetMode(Input(), 1);    /* RAW console: no echo, no line editing */
    for (;;) {
        got = Read(Input(), &ch, 1);
        if (got <= 0) {
            SetMode(Input(), 0);
            return -1;
        }
        tg_os3_note_input_event((int)ch);
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (pos > 0UL) {
                --pos;
            }
            continue;
        }
        if (pos + 1UL < out_size) {
            out[pos++] = ch;
        }
    }
    out[pos] = '\0';
    SetMode(Input(), 0);    /* restore cooked mode */
    return 0;
#else
    if (out != 0 && out_size > 0UL) {
        out[0] = '\0';
    }
    return -1;
#endif
}

int tg_platform_stdin_set_raw(int enabled)
{
#if defined(__amigaos3__)
    if (SetMode(Input(), enabled ? 1 : 0)) {
        return 0;
    }
    return -1;
#else
    (void)enabled;
    return -1;
#endif
}

#if defined(__amigaos3__)
/*
 * In-tree CSPRNG for AmigaOS 3.x (SHA-256 Hash-DRBG), so the m68k client needs
 * NO AmiSSL for randomness. Mirrors the AmigaOS 4 construction; the only
 * platform difference is the high-resolution entropy source: timer.device's
 * E-Clock (ReadEClock) sampled in a variable-duration jitter loop, plus the
 * coarse clock, several run-varying addresses and a per-call counter. Output is
 * SHA-256(key || counter); the persistent state is ratcheted forward after each
 * call (SHA-256(key || 0x01)) so earlier output cannot be reconstructed.
 *
 * SECURITY NOTE: validate entropy on real m68k hardware via --platform-rng-test
 * before trusting this for production logins.
 */
struct Device *TimerBase = 0;           /* used by the proto/timer.h inlines */
static struct timerequest tg_os3_timereq;
static struct MsgPort *tg_os3_timeport = 0;
static int tg_os3_timer_tried = 0;
static unsigned char tg_os3_drbg_state[TG_MTPROTO_SHA256_LENGTH];
static int tg_os3_drbg_ready = 0;

static void tg_os3_timer_open(void)
{
    if (tg_os3_timer_tried) {
        return;
    }
    tg_os3_timer_tried = 1;
    tg_os3_timeport = CreateMsgPort();
    if (tg_os3_timeport == 0) {
        return;
    }
    memset(&tg_os3_timereq, 0, sizeof(tg_os3_timereq));
    tg_os3_timereq.tr_node.io_Message.mn_ReplyPort = tg_os3_timeport;
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)&tg_os3_timereq, 0) == 0) {
        TimerBase = tg_os3_timereq.tr_node.io_Device;
    }
}

static unsigned long tg_os3_timebase(void)
{
    struct EClockVal ev;
    if (TimerBase == 0) {
        return (unsigned long)time(0);
    }
    ReadEClock(&ev);
    return (unsigned long)ev.ev_lo;
}

static unsigned long tg_os3_entropy_gather(unsigned char *buf, unsigned long cap)
{
    unsigned long n = 0;
    unsigned long i;
    unsigned long t;
    void *p;
    struct Task *task;

    tg_os3_timer_open();

    t = (unsigned long)time(0);
    if (n + sizeof(t) <= cap) { memcpy(buf + n, &t, sizeof(t)); n += sizeof(t); }

    for (i = 0; i < 128UL && n + sizeof(unsigned long) <= cap; ++i) {
        struct EClockVal ev_j;
        unsigned long tb, tbhi = 0;
        volatile unsigned long spin;
        unsigned long k;
        if (TimerBase != 0) {
            ReadEClock(&ev_j);
            tb   = (unsigned long)ev_j.ev_lo;
            tbhi = (unsigned long)ev_j.ev_hi;
        } else {
            tb = (unsigned long)time(0);
        }
        memcpy(buf + n, &tb, sizeof(tb));
        n += sizeof(tb);
        if (n + sizeof(tbhi) <= cap) { memcpy(buf + n, &tbhi, sizeof(tbhi)); n += sizeof(tbhi); }
        spin = tb;
        for (k = 0; k < (tb & 0x3fUL); ++k) {
            spin = (spin * 2654435761UL) + k;
        }
        (void)spin;
    }

    p = (void *)&n;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    p = (void *)buf;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    p = (void *)SysBase;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    task = FindTask(0);
    if (n + sizeof(task) <= cap) {
        memcpy(buf + n, &task, sizeof(task));
        n += sizeof(task);
    }
    {
        /* AvailMem reflects runtime allocation state; harder to predict than
           static addresses alone, especially across different boot sessions. */
        unsigned long chip = (unsigned long)AvailMem(MEMF_CHIP);
        unsigned long any  = (unsigned long)AvailMem(MEMF_ANY);
        if (n + sizeof(chip) <= cap) { memcpy(buf + n, &chip, sizeof(chip)); n += sizeof(chip); }
        if (n + sizeof(any)  <= cap) { memcpy(buf + n, &any,  sizeof(any));  n += sizeof(any);  }
    }

    return n;
}

static void tg_os3_drbg_generate(unsigned char *out, unsigned long n);

static FILE *tg_os3_open_seed_file(const char *mode)
{
    /* PROGDIR: keeps the seed next to the binary; some C libraries do not
       grok Amiga-style paths, so fall back to the current directory (the
       icon launcher CDs into the drawer anyway). */
    FILE *f = fopen("PROGDIR:telegram-seed.bin", mode);
    if (f == 0) {
        f = fopen("telegram-seed.bin", mode);
    }
    return f;
}

/*
 * Persistent seed (PROGDIR:telegram-seed.bin, Linux random-seed style):
 * entropy accumulates across runs instead of restarting from a cold,
 * reproducible boot state. The file is mixed into the pool at seed time
 * and immediately overwritten with fresh DRBG output, so yesterday's file
 * never predicts today's state.
 */
static void tg_os3_drbg_seed(void)
{
    unsigned char pool[1024];
    unsigned long len = tg_os3_entropy_gather(pool, sizeof(pool));
    {
        FILE *seed_file = tg_os3_open_seed_file("rb");
        if (seed_file != 0) {
            unsigned char saved[64];
            unsigned long got = (unsigned long)fread(saved, 1U,
                                                     sizeof(saved),
                                                     seed_file);
            fclose(seed_file);
            if (got > 0UL && len + got <= sizeof(pool)) {
                memcpy(pool + len, saved, got);
                len += got;
            }
        }
    }
    tg_mtproto_sha256(pool, len, tg_os3_drbg_state);
    tg_os3_drbg_ready = 1;
    {
        unsigned char fresh[64];
        FILE *seed_file;
        tg_os3_drbg_generate(fresh, sizeof(fresh));
        seed_file = tg_os3_open_seed_file("wb");
        if (seed_file != 0) {
            fwrite(fresh, 1U, sizeof(fresh), seed_file);
            fclose(seed_file);
        }
        memset(fresh, 0, sizeof(fresh));
    }
}

static void tg_os3_drbg_generate(unsigned char *out, unsigned long n)
{
    static unsigned long calls = 0;
    unsigned char key[TG_MTPROTO_SHA256_LENGTH];
    unsigned char block[TG_MTPROTO_SHA256_LENGTH];
    unsigned char work[TG_MTPROTO_SHA256_LENGTH + 16 +
                       sizeof(tg_os3_key_ring) + sizeof(unsigned long)];
    unsigned long off;
    unsigned long ctr;
    unsigned long wn;
    unsigned long tb;

    if (!tg_os3_drbg_ready) {
        tg_os3_drbg_seed();
    }

    wn = TG_MTPROTO_SHA256_LENGTH;
    tb = tg_os3_timebase();
    ++calls;
    memcpy(work, tg_os3_drbg_state, TG_MTPROTO_SHA256_LENGTH);
    memcpy(work + wn, &tb, sizeof(tb));
    wn += sizeof(tb);
    memcpy(work + wn, &calls, sizeof(calls));
    wn += sizeof(calls);
    /* Keystroke-timing ring: human input collected since the last
       generate (cheap on the input path, absorbed here). */
    memcpy(work + wn, tg_os3_key_ring, sizeof(tg_os3_key_ring));
    wn += sizeof(tg_os3_key_ring);
    memcpy(work + wn, &tg_os3_key_ring_pos, sizeof(tg_os3_key_ring_pos));
    wn += sizeof(tg_os3_key_ring_pos);
    tg_mtproto_sha256(work, wn, key);

    off = 0;
    ctr = 0;
    while (off < n) {
        unsigned char cb[TG_MTPROTO_SHA256_LENGTH + 4];
        unsigned long take;
        memcpy(cb, key, TG_MTPROTO_SHA256_LENGTH);
        cb[TG_MTPROTO_SHA256_LENGTH + 0] = (unsigned char)((ctr >> 24) & 0xffUL);
        cb[TG_MTPROTO_SHA256_LENGTH + 1] = (unsigned char)((ctr >> 16) & 0xffUL);
        cb[TG_MTPROTO_SHA256_LENGTH + 2] = (unsigned char)((ctr >> 8) & 0xffUL);
        cb[TG_MTPROTO_SHA256_LENGTH + 3] = (unsigned char)(ctr & 0xffUL);
        tg_mtproto_sha256(cb, sizeof(cb), block);
        take = n - off;
        if (take > TG_MTPROTO_SHA256_LENGTH) {
            take = TG_MTPROTO_SHA256_LENGTH;
        }
        memcpy(out + off, block, take);
        off += take;
        ++ctr;
    }

    {
        unsigned char rb[TG_MTPROTO_SHA256_LENGTH + 1];
        memcpy(rb, key, TG_MTPROTO_SHA256_LENGTH);
        rb[TG_MTPROTO_SHA256_LENGTH] = 0x01;
        tg_mtproto_sha256(rb, sizeof(rb), tg_os3_drbg_state);
    }
}
#endif /* __amigaos3__ */

int tg_platform_random_bytes(unsigned char *bytes, unsigned long byte_count)
{
#if defined(__amigaos3__)
    if (bytes == 0) {
        return 0;
    }
    if (byte_count == 0) {
        return 1;
    }
    tg_os3_drbg_generate(bytes, byte_count);
    return 1;
#else
    int fd;
    unsigned long offset;
    long got;

    if (bytes == 0) {
        return 0;
    }
    if (byte_count == 0) {
        return 1;
    }
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        fd = open("/dev/random", O_RDONLY);
    }
    if (fd < 0) {
        return 0;
    }
    offset = 0;
    while (offset < byte_count) {
        got = read(fd, bytes + offset, byte_count - offset);
        if (got <= 0) {
            close(fd);
            return 0;
        }
        offset += (unsigned long)got;
    }
    close(fd);
    return 1;
#endif
}

#if defined(__amigaos3__)

static void tg_platform_set_error(char *error_buffer, unsigned long error_buffer_size,
                                  const char *message)
{
    if (error_buffer != 0 && error_buffer_size > 0) {
        snprintf(error_buffer, error_buffer_size, "%s", message);
    }
}

#if TG_AMIGAOS3_BSDSOCKET_DIRECT
static int tg_amigaos3_socket_open(char *error_buffer, unsigned long error_buffer_size)
{
    if (SocketBase != 0) {
        return 0;
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not open bsdsocket.library v4");
        return 1;
    }

    SetErrnoPtr(&errno, sizeof(errno));
    return 0;
}
#endif

tg_net_status tg_platform_tcp_connect(tg_net_connection *connection, const char *host,
                                      const char *port, char *error_buffer,
                                      unsigned long error_buffer_size)
{
    struct hostent *host_entry;
    struct sockaddr_in address;
    long port_number;
    int sock;
    int rc;

    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    port_number = strtol(port, 0, 10);
    if (port_number <= 0 || port_number > 65535) {
        return TG_NET_INVALID_ARGUMENT;
    }

#if TG_AMIGAOS3_BSDSOCKET_DIRECT
    if (tg_amigaos3_socket_open(error_buffer, error_buffer_size) != 0) {
        return TG_NET_CONNECT_FAILED;
    }
#endif

#if TG_AMIGAOS3_BSDSOCKET_DIRECT
    host_entry = (struct hostent *)gethostbyname((STRPTR)host);
#else
    host_entry = gethostbyname((char *)host);
#endif
    if (host_entry == 0 || host_entry->h_addr_list == 0 ||
        host_entry->h_addr_list[0] == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, "host lookup failed");
        return TG_NET_RESOLVE_FAILED;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port_number);
    memcpy(&address.sin_addr, host_entry->h_addr_list[0], sizeof(address.sin_addr));

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, "socket open failed");
        return TG_NET_CONNECT_FAILED;
    }

    rc = connect(sock, (struct sockaddr *)&address, sizeof(address));
    if (rc == 0) {
        connection->platform_handle = sock;
        connection->is_open = 1;
        return TG_NET_OK;
    }

#if TG_AMIGAOS3_BSDSOCKET_DIRECT
    CloseSocket((long)sock);
#else
    close(sock);
#endif
    tg_platform_set_error(error_buffer, error_buffer_size, "socket connect failed");
    return TG_NET_CONNECT_FAILED;
}

tg_net_status tg_platform_tcp_send(tg_net_connection *connection, const void *data,
                                   unsigned long byte_count, unsigned long *bytes_sent,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    long rc;

    if (bytes_sent != 0) {
        *bytes_sent = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    rc = send(connection->platform_handle, (void *)data, (long)byte_count, 0);
    if (rc < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, "socket send failed");
        return TG_NET_SEND_FAILED;
    }
    if (bytes_sent != 0) {
        *bytes_sent = (unsigned long)rc;
    }
    return TG_NET_OK;
}

tg_net_status tg_platform_tcp_recv(tg_net_connection *connection, void *buffer,
                                   unsigned long buffer_size, unsigned long *bytes_received,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    long rc;

    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

#if TG_AMIGAOS3_BSDSOCKET_DIRECT
    /* Bound the blocking recv() with a bsdsocket WaitSelect() timeout (see the
       detailed note at the WaitSelect call below for why it must NOT be ixemul
       select()). Without a bounded wait the encrypted-query receive loop in
       tg_mtproto_send_saved_query_on_context() can block forever inside recv()
       when a long-idle TCP connection is dropped silently (no FIN): its ~12s
       wall-clock budget is only checked between reads, so a stuck recv() never
       lets it fire and the chat session freezes after a while.

       WaitSelect() takes the devices/timer.h `struct timeval` (whose
       tv_sec/tv_usec are union aliases of tv_secs/tv_micro) already in scope via
       proto/dos.h, so no <sys/time.h> is pulled in (which would clash with
       devices/timer.h). */
    {
        unsigned long timeout_seconds;
        fd_set read_fds;
        struct timeval timeout;
        long sel;

        timeout_seconds = tg_net_connect_timeout_seconds();
        if (timeout_seconds == 0UL) {
            timeout_seconds = 30UL;
        }
        FD_ZERO(&read_fds);
        FD_SET((int)connection->platform_handle, &read_fds);
        timeout.tv_sec = (long)timeout_seconds;
        timeout.tv_usec = 0;
        /* MUST be bsdsocket WaitSelect(), NOT ixemul select(): the socket is a
           bsdsocket descriptor living in its OWN fd namespace (typically the
           first socket gets descriptor 0). ixemul's select() interprets that fd
           number in ITS namespace, so FD_SET(socket==0) ends up monitoring
           ixemul stdin (fd 0 = the console), not the socket. That made the
           interactive login wizard hang forever on res_pq right after the
           "Phone number:" prompt: once fgets() drained the console, select()
           waited on the console (which never gets more input) while the real
           reply sat unread on the socket. Non-interactive probes only "worked"
           by luck because their stdin pipe was at EOF (which reads as always
           ready). WaitSelect() lives in the bsdsocket fd namespace, so FD_SET()
           and the wait both refer to the socket. */
        sel = WaitSelect((int)connection->platform_handle + 1, &read_fds, 0, 0,
                         (void *)&timeout, 0);
        if (sel <= 0 || !FD_ISSET((int)connection->platform_handle, &read_fds)) {
            tg_platform_set_error(error_buffer, error_buffer_size,
                                  sel == 0 ? "socket receive timed out"
                                           : "socket receive failed");
            return sel == 0 ? TG_NET_TIMEOUT : TG_NET_RECV_FAILED;
        }
    }
#endif

    rc = recv(connection->platform_handle, buffer, (long)buffer_size, 0);
    if (rc < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, "socket receive failed");
        return TG_NET_RECV_FAILED;
    }
    if (rc == 0) {
        return TG_NET_CLOSED;
    }
    if (bytes_received != 0) {
        *bytes_received = (unsigned long)rc;
    }
    return TG_NET_OK;
}

void tg_platform_tcp_close(tg_net_connection *connection)
{
    if (connection != 0 && connection->is_open) {
#if TG_AMIGAOS3_BSDSOCKET_DIRECT
        CloseSocket((long)connection->platform_handle);
#if TG_AMIGAOS3_ENABLE_AMISSL
        if (!tg_amigaos3_amissl_initialized && SocketBase != 0) {
            CloseLibrary(SocketBase);
            SocketBase = 0;
        }
#else
        if (SocketBase != 0) {
            CloseLibrary(SocketBase);
            SocketBase = 0;
        }
#endif
#else
        close((int)connection->platform_handle);
#endif
    }
}

#else

tg_net_status tg_platform_tcp_connect(tg_net_connection *connection, const char *host,
                                      const char *port, char *error_buffer,
                                      unsigned long error_buffer_size)
{
    (void)connection;
    (void)host;
    (void)port;
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    return TG_NET_UNSUPPORTED;
}

tg_net_status tg_platform_tcp_send(tg_net_connection *connection, const void *data,
                                   unsigned long byte_count, unsigned long *bytes_sent,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    (void)connection;
    (void)data;
    (void)byte_count;
    if (bytes_sent != 0) {
        *bytes_sent = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    return TG_NET_UNSUPPORTED;
}

tg_net_status tg_platform_tcp_recv(tg_net_connection *connection, void *buffer,
                                   unsigned long buffer_size, unsigned long *bytes_received,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    (void)connection;
    (void)buffer;
    (void)buffer_size;
    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    return TG_NET_UNSUPPORTED;
}

void tg_platform_tcp_close(tg_net_connection *connection)
{
    (void)connection;
}

#endif

#if defined(__amigaos3__) && TG_AMIGAOS3_ENABLE_AMISSL

static void tg_platform_set_ssl_error(char *error_buffer, unsigned long error_buffer_size,
                                      const char *fallback_message)
{
    unsigned long error_code;
    const char *error_string;

    error_code = ERR_get_error();
    if (error_code != 0) {
        error_string = ERR_reason_error_string(error_code);
        if (error_string != 0) {
            tg_platform_set_error(error_buffer, error_buffer_size, error_string);
            return;
        }
    }

    tg_platform_set_error(error_buffer, error_buffer_size, fallback_message);
}

static tg_tls_status tg_amigaos3_configure_certificate_validation(
    SSL_CTX *ctx,
    SSL *ssl,
    const char *host,
    char *error_buffer,
    unsigned long error_buffer_size)
{
    const char *ca_file;
    const char *ca_path;

    if (!tg_tls_certificate_validation_enabled()) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);
        return TG_TLS_OK;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, 0);
    ca_file = tg_tls_certificate_ca_file();
    ca_path = tg_tls_certificate_ca_path();
    if (ca_file != 0 || ca_path != 0) {
        if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_path) != 1) {
            tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                      "could not load CA file/path");
            return TG_TLS_VERIFY_FAILED;
        }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "could not load default CA paths");
        return TG_TLS_VERIFY_FAILED;
    }

    if (SSL_set1_host(ssl, host) != 1) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "could not enable hostname verification");
        return TG_TLS_VERIFY_FAILED;
    }

    return TG_TLS_OK;
}

static void tg_amigaos3_amissl_cleanup(void)
{
    if (tg_amigaos3_amissl_initialized) {
        CleanupAmiSSLA(0);
        tg_amigaos3_amissl_initialized = 0;
    }

    if (AmiSSLBase != 0) {
        CloseAmiSSL();
        AmiSSLBase = 0;
        AmiSSLExtBase = 0;
    }

    if (AmiSSLMasterBase != 0) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = 0;
    }

    if (SocketBase != 0) {
        CloseLibrary(SocketBase);
        SocketBase = 0;
    }
}

static int tg_amigaos3_amissl_init(char *error_buffer, unsigned long error_buffer_size)
{
    long amissl_error;
    char detail[80];

    if (tg_amigaos3_amissl_initialized) {
        return 0;
    }

    if (tg_amigaos3_socket_open(error_buffer, error_buffer_size) != 0) {
        tg_amigaos3_amissl_cleanup();
        return 1;
    }

    AmiSSLMasterBase = OpenLibrary((CONST_STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not open amisslmaster.library");
        tg_amigaos3_amissl_cleanup();
        return 1;
    }

    amissl_error = OpenAmiSSLTags(TG_AMIGAOS3_AMISSL_API_VERSION,
                                  AmiSSL_UsesOpenSSLStructs, FALSE,
                                  AmiSSL_InitAmiSSL, TRUE,
                                  AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                                  AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                                  AmiSSL_SocketBase, (ULONG)SocketBase,
                                  AmiSSL_ErrNoPtr, (ULONG)&errno,
                                  TAG_DONE);
    if (amissl_error != 0) {
        snprintf(detail, sizeof(detail), "could not initialize AmiSSL (%ld)", amissl_error);
        tg_platform_set_error(error_buffer, error_buffer_size, detail);
        tg_amigaos3_amissl_cleanup();
        return 1;
    }

    tg_amigaos3_amissl_initialized = 1;
    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT | OPENSSL_INIT_ADD_ALL_CIPHERS |
                         OPENSSL_INIT_ADD_ALL_DIGESTS,
                     0);
    return 0;
}

tg_tls_status tg_platform_tls_connect(tg_tls_connection *connection, const char *host,
                                      const char *port, tg_net_status *net_status,
                                      char *error_buffer, unsigned long error_buffer_size)
{
    SSL_CTX *ctx;
    SSL *ssl;
    tg_tls_status verify_status;
    tg_net_status local_net_status;

    if (net_status != 0) {
        *net_status = TG_NET_OK;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    if (tg_amigaos3_amissl_init(error_buffer, error_buffer_size) != 0) {
        return TG_TLS_HANDSHAKE_FAILED;
    }

    local_net_status = tg_net_connect(&connection->tcp, host, port,
                                      error_buffer, error_buffer_size);
    if (local_net_status != TG_NET_OK) {
        if (net_status != 0) {
            *net_status = local_net_status;
        }
        tg_amigaos3_amissl_cleanup();
        return TG_TLS_NET_ERROR;
    }

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "could not create SSL context");
        tg_net_close(&connection->tcp);
        tg_amigaos3_amissl_cleanup();
        return TG_TLS_HANDSHAKE_FAILED;
    }

#ifdef SSL_MODE_AUTO_RETRY
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
#endif

    ssl = SSL_new(ctx);
    if (ssl == 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "could not create SSL session");
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos3_amissl_cleanup();
        return TG_TLS_HANDSHAKE_FAILED;
    }

    SSL_set_fd(ssl, (int)connection->tcp.platform_handle);
    SSL_set_tlsext_host_name(ssl, host);
    verify_status = tg_amigaos3_configure_certificate_validation(
        ctx, ssl, host, error_buffer, error_buffer_size);
    if (verify_status != TG_TLS_OK) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos3_amissl_cleanup();
        return verify_status;
    }
    ERR_clear_error();

    if (SSL_connect(ssl) != 1) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "SSL handshake failed");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos3_amissl_cleanup();
        return TG_TLS_HANDSHAKE_FAILED;
    }
    if (tg_tls_certificate_validation_enabled() &&
        SSL_get_verify_result(ssl) != X509_V_OK) {
        tg_platform_set_error(
            error_buffer, error_buffer_size,
            X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos3_amissl_cleanup();
        return TG_TLS_VERIFY_FAILED;
    }

    connection->platform_context = ctx;
    connection->platform_session = ssl;
    connection->is_open = 1;
    return TG_TLS_OK;
}

tg_tls_status tg_platform_tls_send(tg_tls_connection *connection, const void *data,
                                   unsigned long byte_count, unsigned long *bytes_sent,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    int rc;

    if (bytes_sent != 0) {
        *bytes_sent = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    rc = SSL_write((SSL *)connection->platform_session, data, (int)byte_count);
    if (rc <= 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "SSL write failed");
        return TG_TLS_SEND_FAILED;
    }

    if (bytes_sent != 0) {
        *bytes_sent = (unsigned long)rc;
    }
    return TG_TLS_OK;
}

tg_tls_status tg_platform_tls_recv(tg_tls_connection *connection, void *buffer,
                                   unsigned long buffer_size, unsigned long *bytes_received,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    int rc;
    int ssl_error;

    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    rc = SSL_read((SSL *)connection->platform_session, buffer, (int)buffer_size);
    if (rc > 0) {
        if (bytes_received != 0) {
            *bytes_received = (unsigned long)rc;
        }
        return TG_TLS_OK;
    }

    ssl_error = SSL_get_error((SSL *)connection->platform_session, rc);
    if (rc == 0 || ssl_error == SSL_ERROR_ZERO_RETURN) {
        return TG_TLS_CLOSED;
    }

    tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                              "SSL read failed");
    return TG_TLS_RECV_FAILED;
}

void tg_platform_tls_close(tg_tls_connection *connection)
{
    SSL *ssl;
    SSL_CTX *ctx;

    if (connection == 0) {
        return;
    }

    ssl = (SSL *)connection->platform_session;
    ctx = (SSL_CTX *)connection->platform_context;
    if (ssl != 0) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ctx != 0) {
        SSL_CTX_free(ctx);
    }

    tg_net_close(&connection->tcp);
    connection->platform_session = 0;
    connection->platform_context = 0;
    connection->is_open = 0;
    tg_amigaos3_amissl_cleanup();
}

#else

tg_tls_status tg_platform_tls_connect(tg_tls_connection *connection, const char *host,
                                      const char *port, tg_net_status *net_status,
                                      char *error_buffer, unsigned long error_buffer_size)
{
    (void)connection;
    (void)host;
    (void)port;
    if (net_status != 0) {
        *net_status = TG_NET_OK;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    return TG_TLS_UNSUPPORTED;
}

tg_tls_status tg_platform_tls_send(tg_tls_connection *connection, const void *data,
                                   unsigned long byte_count, unsigned long *bytes_sent,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    (void)connection;
    (void)data;
    (void)byte_count;
    if (bytes_sent != 0) {
        *bytes_sent = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    return TG_TLS_UNSUPPORTED;
}

tg_tls_status tg_platform_tls_recv(tg_tls_connection *connection, void *buffer,
                                   unsigned long buffer_size, unsigned long *bytes_received,
                                   char *error_buffer, unsigned long error_buffer_size)
{
    (void)connection;
    (void)buffer;
    (void)buffer_size;
    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
    return TG_TLS_UNSUPPORTED;
}

void tg_platform_tls_close(tg_tls_connection *connection)
{
    (void)connection;
}

#endif

int tg_platform_break_pending(void)
{
#if defined(__amigaos3__)
    /* Peek without clearing: the break stays pending for outer loops. */
    return (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) != 0L;
#else
    return 0;
#endif
}

#if defined(__amigaos3__)
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase = 0;
#endif

void tg_platform_display_beep(void)
{
#if defined(__amigaos3__)
    /* Open per call: intuition is resident, the open is a refcount bump.
       The screen flash is the Amiga-native notification; sending a BEL byte
       instead lets console handlers improvise (AmiKit's clears the window). */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library",
                                                        0L);
    if (IntuitionBase != 0) {
        DisplayBeep(0L);
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
    }
#endif
}
