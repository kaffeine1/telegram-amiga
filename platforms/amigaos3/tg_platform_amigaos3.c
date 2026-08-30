/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <sys/stat.h>
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
#include <dos/dosextens.h>
#include <dos/dostags.h> /* SYS_Input/SYS_Output for the URL opener */
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

#if defined(__AROS__) || defined(__amigaos4__) || defined(__MORPHOS__) || defined(__amigaos3__) || defined(__m68k__)
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/icon.h>
#include <proto/wb.h>
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

static int tg_amigaos3_amissl_init(char *error_buffer,
                                   unsigned long error_buffer_size);
#endif

#if defined(__amigaos3__) && TG_AMIGAOS3_NOIXEMUL && !TG_AMIGAOS3_ENABLE_AMISSL
/* bsdsocket.library base used by the proto/socket.h inlines (no ixemul to
   provide BSD sockets). Opened in tg_amigaos3_socket_open(). */
struct Library *SocketBase = 0;
#endif

const char *tg_platform_name(void)
{
    return "AmigaOS 3.x";
}

const char *tg_platform_default_data_dir(void)
{
    return "PROGDIR:";
}

int tg_platform_run_with_safe_stack(tg_platform_entry_fn entry,
                                    int argc, char **argv)
{
#if defined(__amigaos3__)
    struct Task *task;
    unsigned long current_size;

    task = FindTask(0);
    if (task != 0 && task->tc_SPUpper != 0 && task->tc_SPLower != 0) {
        current_size = (unsigned long)((unsigned char *)task->tc_SPUpper -
                                       (unsigned char *)task->tc_SPLower);
        if (current_size < TG_PLATFORM_SAFE_STACK_MIN) {
            /* Say the real number for THIS build (the 68000 profile asks
               for less) and how to get it: a bare "needs 1 MiB" left a
               field tester stuck at a 4096-byte Shell prompt. */
            fprintf(stderr,
                    "Telegram Amiga needs a %lu byte stack; this one is %lu.\n"
                    "In a Shell type \"stack %lu\" once, then run it again,\n"
                    "or start it from its icon, which sets the stack for you.\n",
                    (unsigned long)TG_PLATFORM_SAFE_STACK_SIZE, current_size,
                    (unsigned long)TG_PLATFORM_SAFE_STACK_SIZE);
            return 20;
        }
    }
#endif
    return entry(argc, argv);
}

unsigned long tg_platform_local_epoch(void)
{
    struct DateStamp ds;

    /* The Amiga system clock is LOCAL wall-clock time (what the Workbench clock
       shows): days/minutes/ticks since 1978-01-01, with no timezone or DST
       concept. Convert to a Unix-style epoch with NO offset applied -- clib2's
       time() instead adds the locale GMT offset (and never DST), so reading the
       battclock directly is the only value that always matches the system clock.
       252460800 = seconds from 1970-01-01 to the 1978-01-01 Amiga epoch. */
    DateStamp(&ds);
    return (unsigned long)ds.ds_Days * 86400UL
         + (unsigned long)ds.ds_Minute * 60UL
         + (unsigned long)ds.ds_Tick / 50UL
         + 252460800UL;
}


/* issue #9: read the launched icon's TUI_MODE tooltype (classic icon.library).
   arg 0 of WBStartup is the icon the user double-clicked. Returns 1 (TUI),
   0 (GUI: NO/FALSE/OFF), -1 (no tooltype / no icon -> caller uses the name
   heuristic). Opening icon.library here is cheap and one-shot at startup. */
int tg_platform_wb_tui_mode(char **argv)
{
    struct WBStartup *wb = (struct WBStartup *)argv;
    struct Library *IconBase;
    struct WBArg *a;
    struct DiskObject *dobj;
    int result = -1;

    if (wb == 0 || wb->sm_ArgList == 0 || wb->sm_NumArgs < 1) {
        return -1;
    }
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36L);
    if (IconBase == 0) {
        return -1;
    }
    a = &wb->sm_ArgList[0];
    if (a->wa_Name != 0 && a->wa_Name[0] != '\0') {
        BPTR olddir = 0;
        int have_dir = 0;

        if (a->wa_Lock != 0) {
            olddir = CurrentDir(a->wa_Lock);
            have_dir = 1;
        }
        dobj = GetDiskObject((STRPTR)a->wa_Name);
        if (dobj != 0) {
            STRPTR tt = FindToolType(dobj->do_ToolTypes,
                                     (STRPTR)"TUI_MODE");

            if (tt != 0) {
                result = (MatchToolValue(tt, (STRPTR)"NO") ||
                          MatchToolValue(tt, (STRPTR)"FALSE") ||
                          MatchToolValue(tt, (STRPTR)"OFF")) ? 0 : 1;
            }
            FreeDiskObject(dobj);
        }
        if (have_dir) {
            CurrentDir(olddir);
        }
    }
    CloseLibrary(IconBase);
    return result;
}

void tg_platform_workbench_init(void)
{
#if defined(__amigaos3__)
    /* Workbench start has no current drawer set to the binary's home, so the
       relative data files would miss. Anchor the CWD to PROGDIR:. The lock is
       held for the process lifetime (the OS frees it at exit). */
    BPTR progdir = Lock((CONST_STRPTR)"PROGDIR:", SHARED_LOCK);
    if (progdir != 0) {
        CurrentDir(progdir);
    }
#endif
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

/* Public GUI hook: same ring, full event words (IDCMP class^code + mouse
   coords) instead of one console byte. The E-Clock read at call time carries
   the actual entropy; the words decorrelate identical-timing events. */
void tg_platform_note_input_event(unsigned long a, unsigned long b)
{
#if defined(__amigaos3__)
    unsigned long v = tg_os3_timebase() ^ a ^ (b << 13) ^ (b >> 7) ^
                      (tg_os3_key_ring_pos * 2654435761UL);
    tg_os3_key_ring[tg_os3_key_ring_pos & 15UL] ^=
        (v << (tg_os3_key_ring_pos & 7UL)) ^ (v >> 5);
    ++tg_os3_key_ring_pos;
#else
    (void)a;
    (void)b;
#endif
}

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
static int tg_os3_timer_opened = 0;
static unsigned char tg_os3_drbg_state[TG_MTPROTO_SHA256_LENGTH];
static int tg_os3_drbg_ready = 0;

static void tg_os3_timer_close(void)
{
    if (tg_os3_timer_opened) {
        CloseDevice((struct IORequest *)&tg_os3_timereq);
        tg_os3_timer_opened = 0;
    }
    TimerBase = 0;
    if (tg_os3_timeport != 0) {
        DeleteMsgPort(tg_os3_timeport);
        tg_os3_timeport = 0;
    }
}

static void tg_os3_timer_open(void)
{
    if (tg_os3_timer_tried) {
        return;
    }
    tg_os3_timer_tried = 1;
    tg_os3_timeport = CreateMsgPort();
    if (tg_os3_timeport == 0) {
        /* Without timer.device the entropy timebase degrades to time(0) at 1s
           granularity and the jitter loop flattens -- say so instead of
           degrading silently (rng security note, 2026-06-21 review). */
        puts("platform rng: WARNING no msg port - E-Clock entropy degraded");
        return;
    }
    memset(&tg_os3_timereq, 0, sizeof(tg_os3_timereq));
    tg_os3_timereq.tr_node.io_Message.mn_ReplyPort = tg_os3_timeport;
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)&tg_os3_timereq, 0) == 0) {
        TimerBase = tg_os3_timereq.tr_node.io_Device;
        tg_os3_timer_opened = 1;
    } else {
        puts("platform rng: WARNING timer.device failed - E-Clock entropy degraded");
    }
}

static void tg_os3_socket_shutdown(void);

void tg_platform_shutdown(void)
{
    tg_os3_timer_close();
    tg_os3_socket_shutdown();
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
        unsigned long tb = tg_os3_timebase();
        volatile unsigned long spin;
        unsigned long k;
        memcpy(buf + n, &tb, sizeof(tb));
        n += sizeof(tb);
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

    return n;
}

static void tg_os3_drbg_generate(unsigned char *out, unsigned long n);

static FILE *tg_os3_open_seed_file(const char *mode)
{
    /* PROGDIR: keeps the seed next to the binary; some C libraries do not
       grok Amiga-style paths, so fall back to the current directory (the
       icon launcher CDs into the drawer anyway). */
    FILE *f;

    /* Tidy layout: the seed lives in data/ with the other auxiliary files.
       One-time migration of a root-era seed, with the data/ copy winning
       (never overwrite live state with a stale root leftover). */
    f = fopen("PROGDIR:data/telegram-seed.bin", "rb");
    if (f == 0) {
        f = fopen("data/telegram-seed.bin", "rb");
    }
    if (f != 0) {
        fclose(f);
        (void)remove("PROGDIR:telegram-seed.bin");
        (void)remove("telegram-seed.bin");
    } else {
        (void)mkdir("data", 0777);
        if (rename("PROGDIR:telegram-seed.bin",
                   "PROGDIR:data/telegram-seed.bin") != 0) {
            (void)rename("telegram-seed.bin", "data/telegram-seed.bin");
        }
    }
    f = fopen("PROGDIR:data/telegram-seed.bin", mode);
    if (f == 0) {
        f = fopen("data/telegram-seed.bin", mode);
    }
    return f;
}

/*
 * Persistent seed (PROGDIR:data/telegram-seed.bin, Linux random-seed style):
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
        strncpy(error_buffer, message, error_buffer_size - 1);
        error_buffer[error_buffer_size - 1] = '\0';
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

    /* Bound the blocking send() with a writability wait, mirroring the recv
       timeout: a wedged link (QEMU slirp stalls on sustained uploads) left
       send() blocked FOREVER -- upload frozen at N%, no error, cancel dead.
       A stalled socket now surfaces as a timeout, so the per-part retry (on a
       fresh connection) or a clean abort takes over. */
#if TG_AMIGAOS3_BSDSOCKET_DIRECT
    {
        unsigned long timeout_seconds = tg_net_connect_timeout_seconds();
        fd_set write_fds;
        struct timeval timeout;
        long src;

        if (timeout_seconds == 0UL) {
            timeout_seconds = 30UL;
        }
        FD_ZERO(&write_fds);
        FD_SET((int)connection->platform_handle, &write_fds);
        timeout.tv_sec = (long)timeout_seconds;
        timeout.tv_usec = 0;
        src = WaitSelect((int)connection->platform_handle + 1, 0, &write_fds,
                         0, &timeout, 0);
        if (src <= 0 || !FD_ISSET((int)connection->platform_handle,
                                  &write_fds)) {
            tg_platform_set_error(error_buffer, error_buffer_size,
                                  src == 0 ? "socket send timed out"
                                           : "socket send wait failed");
            return src == 0 ? TG_NET_TIMEOUT : TG_NET_SEND_FAILED;
        }
    }
#endif
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

int tg_platform_tcp_poll_readable(tg_net_connection *connection,
                                  char *error_buffer,
                                  unsigned long error_buffer_size)
{
    fd_set read_fds;
    struct timeval timeout;
    long rc;

    if (error_buffer != 0 && error_buffer_size > 0UL) {
        error_buffer[0] = '\0';
    }
    if (connection == 0 || !connection->is_open) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "socket is not open");
        return -1;
    }
    FD_ZERO(&read_fds);
    FD_SET((int)connection->platform_handle, &read_fds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
#if TG_AMIGAOS3_BSDSOCKET_DIRECT
    rc = WaitSelect((int)connection->platform_handle + 1, &read_fds, 0, 0,
                    (void *)&timeout, 0);
#else
    rc = select((int)connection->platform_handle + 1, &read_fds, 0, 0,
                &timeout);
#endif
    if (rc < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "socket poll failed");
        return -1;
    }
    return rc > 0 &&
           FD_ISSET((int)connection->platform_handle, &read_fds) ? 1 : 0;
}

void tg_platform_tcp_close(tg_net_connection *connection)
{
    if (connection != 0 && connection->is_open) {
#if TG_AMIGAOS3_BSDSOCKET_DIRECT
        CloseSocket((long)connection->platform_handle);
#else
        close((int)connection->platform_handle);
#endif
        connection->is_open = 0;
    }
    /* bsdsocket.library stays open until tg_platform_shutdown(): closing it
       here zeroed the shared SocketBase under the OTHER live connections (the
       GUI keeps several open), and their next send()/recv() jumped through a
       NULL library base. Found as a reproducible relaunch bus-fault on AROS
       x86_64; the m68k lane had the identical pattern. */
}

/* Process-exit mirror of the lazy OpenLibrary in tcp_connect. When AmiSSL owns
   the base its own teardown closes it (tg_amigaos3_amissl_shutdown). */
static void tg_os3_socket_shutdown(void)
{
#if TG_AMIGAOS3_BSDSOCKET_DIRECT
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
#endif
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

int tg_platform_tcp_poll_readable(tg_net_connection *connection,
                                  char *error_buffer,
                                  unsigned long error_buffer_size)
{
    (void)connection;
    if (error_buffer != 0 && error_buffer_size > 0UL) {
        error_buffer[0] = '\0';
    }
    return -1;
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
        sprintf(detail, "could not initialize AmiSSL (%ld)", amissl_error);
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

/* Clear the "e" protection bit on a completed download (issue #15). The RWED
   bits are active low, so a file is runnable when FIBB_EXECUTE is CLEAR;
   read-modify-write keeps the archive bit and the others as the filesystem
   left them. Any failure is silent: the download itself already succeeded and
   the user can still `protect +e` by hand. */
void tg_platform_set_executable(const char *path)
{
#if defined(__amigaos3__)
    BPTR lock;
    struct FileInfoBlock *fib;

    if (path == 0 || path[0] == '\0') {
        return;
    }
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == 0) {
        return;
    }
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
    if (fib != 0) {
        if (Examine(lock, fib) && fib->fib_DirEntryType < 0) {
            ULONG prot = (ULONG)fib->fib_Protection;

            UnLock(lock);
            lock = 0;
            SetProtection((CONST_STRPTR)path,
                          (LONG)(prot & ~(ULONG)FIBF_EXECUTE));
        }
        FreeDosObject(DOS_FIB, fib);
    }
    if (lock != 0) {
        UnLock(lock);
    }
#else
    (void)path;
#endif
}

void tg_platform_display_beep(void)
{
#if defined(__amigaos3__)
    /* Open per call: intuition is resident, the open is a refcount bump.
       The screen flash is the Amiga-native notification; sending a BEL byte
       instead lets console handlers improvise (AmiKit's clears the window). */
    /* Respect a base someone else is HOLDING (the console drag-and-drop arms
       intuition for the session): only open/close/zero when it was closed on
       entry, or the holder's CloseLibrary at teardown is skipped and the
       open count leaks (review finding on the drop port). */
    int beep_opened = 0;

    if (IntuitionBase == 0) {
        IntuitionBase = (struct IntuitionBase *)
            OpenLibrary("intuition.library", 0L);
        beep_opened = 1;
    }
    if (IntuitionBase != 0) {
        DisplayBeep(0L);
        if (beep_opened) {
            CloseLibrary((struct Library *)IntuitionBase);
            IntuitionBase = 0;
        }
    }
#endif
}

void tg_platform_ensure_drawer_icon(const char *drawer)
{
    struct Library *IconBase;
    struct DiskObject *dobj;

    if (drawer == 0 || drawer[0] == '\0') {
        return;
    }
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36L);
    if (IconBase == 0) {
        return;
    }
    {

        dobj = GetDiskObject((STRPTR)drawer);
        if (dobj != 0) {
            FreeDiskObject(dobj); /* the drawer already has an icon */
        } else {
            dobj = GetDefDiskObject(WBDRAWER);
            if (dobj != 0) {
                dobj->do_CurrentX = NO_ICON_POSITION;
                dobj->do_CurrentY = NO_ICON_POSITION;
                (void)PutDiskObject((STRPTR)drawer, dobj);
                FreeDiskObject(dobj);
            }
        }

    }
    CloseLibrary(IconBase);
}

static void tg_wb_drop_arm(void);
static void tg_wb_drop_disarm(void);

static BPTR tg_wb_tui_con = 0;
static BPTR tg_wb_tui_old_in = 0;
static BPTR tg_wb_tui_old_out = 0;
static struct MsgPort *tg_wb_tui_old_ct = 0;

int tg_platform_workbench_tui_console(void)
{
    BPTR con;

    /* CLOSE keeps the in-chat close gadget (raw event 11 = clean quit). No
       WAIT: its dismissal depends on con-handler behaviour that plain ROM
       3.1 never delivered (two field reports); the farewell pause is ours
       now -- the teardown waits for one keypress, then the window dies
       deterministically with the last Close(). */
    con = Open((CONST_STRPTR)"CON:20/20/640/440/Telegram Amiga TUI/CLOSE",
               MODE_OLDFILE);
    if (con == 0) {
        return 0;
    }
    /* Make this window the process console. SelectInput/SelectOutput switch
       the DOS channels (pr_CIS/pr_COS), but "*" and the C runtime's lazy
       console resolve through pr_ConsoleTask, which a Workbench-launched
       process does NOT have -- without setting it, Open("*") fails, a failed
       freopen() still closes the stdio stream (C89), and the runtime opens
       its own "Output" window on the first write while reads hang: the
       two-window freeze seen on OS3/OS4. SetConsoleTask (dos V36+) points it
       at this CON: handler first. */
    tg_wb_tui_old_ct = (struct MsgPort *)
        SetConsoleTask(((struct FileHandle *)BADDR(con))->fh_Type);
    tg_wb_tui_old_in = SelectInput(con);
    tg_wb_tui_old_out = SelectOutput(con);
    tg_wb_tui_con = con;
    /* Rebind C stdio only if "*" actually resolves now: freopen on a failed
       open would CLOSE stdin/stdout for good, so probe with dos Open first. */
    {
        BPTR probe = Open((CONST_STRPTR)"*", MODE_OLDFILE);

        if (probe != 0) {
            Close(probe);
            (void)freopen("*", "r", stdin);
            (void)freopen("*", "w", stdout);
            (void)freopen("*", "w", stderr);
        }
    }
    tg_wb_drop_arm(); /* best-effort file drag-and-drop */
    return 1;
}

void tg_platform_workbench_tui_console_close(void)
{
    if (tg_wb_tui_con == 0) {
        return; /* console never opened (CLI launch or open failure) */
    }
    tg_wb_drop_disarm();
    /* Farewell pause under OUR control (plain ROM 3.1 never dismissed a
       WAIT window, two field reports): one keypress -- or a close-click
       EOF where the handler provides it -- ends the pause, then every
       handle goes and the window dies with the last Close(). */
    SetMode(Input(), 0);
    {
        char ch;

        (void)Read(Input(), &ch, 1);
    }
    /* Close the freopen'd "*" stdio streams -- nothing may print after
       this point -- then put the original process plumbing back and
       release our own handle. */
    fflush(stdout);
    fflush(stderr);
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    SelectInput(tg_wb_tui_old_in);
    SelectOutput(tg_wb_tui_old_out);
    SetConsoleTask(tg_wb_tui_old_ct);
    Close(tg_wb_tui_con);
    tg_wb_tui_con = 0;
}

/* ---- Workbench TUI drag-and-drop (AppIcon + AppWindow, classic API) ------
   Port of the OS4 lane, proven there in the field: drops on the console
   WINDOW may be owned by the system (they are on OS4), so the reliable lane
   is the "TG drop" APPICON on the Workbench; both feed the same MsgPort.
   Library bases are the shared globals the GUI iconify already uses
   (gui_window.o owns them); we open them only when still closed and close
   only what we opened. */

#if defined(TG_NO_GUI)
/* Text-only build: the GUI window that normally owns these two bases is not
   linked in, but the console still offers Workbench drag-and-drop, so they
   live here instead. */
struct Library *WorkbenchBase = 0;
struct Library *IconBase = 0;
#else
extern struct Library *WorkbenchBase; /* owned by core/tg_gui_window.c */
extern struct Library *IconBase;
#endif

static const char *tg_wb_drop_diag = "not armed";
static unsigned long tg_wb_drop_polls = 0;
static unsigned long tg_wb_drop_msgs = 0;
static struct MsgPort *tg_wb_app_port = 0;
static struct AppWindow *tg_wb_app_win = 0;
static struct AppIcon *tg_wb_app_icon = 0;
static struct DiskObject *tg_wb_drop_dobj = 0;
static int tg_wb_drop_opened_wb = 0;
static int tg_wb_drop_opened_icon = 0;
static int tg_wb_drop_opened_int = 0;

static int tg_wb_window_is_live(struct Window *cand)
{
    struct Screen *scr;
    struct Window *w;
    int found = 0;

    if (cand == 0 || IntuitionBase == 0) {
        return 0;
    }
    scr = LockPubScreen(0);
    if (scr == 0) {
        return 0;
    }
    /* LockPubScreen keeps the screen alive but not its window list. */
    Forbid();
    for (w = scr->FirstWindow; w != 0; w = w->NextWindow) {
        if (w == cand) {
            found = 1;
            break;
        }
    }
    Permit();
    UnlockPubScreen(0, scr);
    return found;
}

static struct Window *tg_wb_find_window_by_title(const char *title)
{
    struct Screen *scr;
    struct Window *w;
    struct Window *found = 0;

    if (IntuitionBase == 0) {
        return 0;
    }
    scr = LockPubScreen(0);
    if (scr == 0) {
        return 0;
    }
    Forbid();
    for (w = scr->FirstWindow; w != 0; w = w->NextWindow) {
        if (w->Title != 0 && strcmp((const char *)w->Title, title) == 0) {
            found = w;
            break;
        }
    }
    Permit();
    UnlockPubScreen(0, scr);
    return found;
}

static void tg_wb_drop_disarm(void)
{
    if (tg_wb_app_icon != 0) {
        RemoveAppIcon(tg_wb_app_icon);
        tg_wb_app_icon = 0;
    }
    if (tg_wb_app_win != 0) {
        RemoveAppWindow(tg_wb_app_win);
        tg_wb_app_win = 0;
    }
    if (tg_wb_app_port != 0) {
        struct Message *m;

        while ((m = GetMsg(tg_wb_app_port)) != 0) {
            ReplyMsg(m);
        }
        DeleteMsgPort(tg_wb_app_port);
        tg_wb_app_port = 0;
    }
    if (tg_wb_drop_dobj != 0 && IconBase != 0) {
        FreeDiskObject(tg_wb_drop_dobj);
        tg_wb_drop_dobj = 0;
    }
    if (tg_wb_drop_opened_icon && IconBase != 0) {
        CloseLibrary(IconBase);
        IconBase = 0;
        tg_wb_drop_opened_icon = 0;
    }
    if (tg_wb_drop_opened_wb && WorkbenchBase != 0) {
        CloseLibrary(WorkbenchBase);
        WorkbenchBase = 0;
        tg_wb_drop_opened_wb = 0;
    }
    if (tg_wb_drop_opened_int && IntuitionBase != 0) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
        tg_wb_drop_opened_int = 0;
    }
}

static void tg_wb_drop_arm(void)
{
    struct InfoData *id;
    struct FileHandle *fh;
    struct Window *win;

    if (tg_wb_tui_con == 0) {
        tg_wb_drop_diag = "no Workbench console";
        return;
    }
    if (tg_wb_app_port != 0 || tg_wb_app_win != 0 || tg_wb_app_icon != 0) {
        return; /* already armed */
    }
    fh = (struct FileHandle *)BADDR(tg_wb_tui_con);
    if (fh == 0) {
        tg_wb_drop_diag = "no console handle";
        return;
    }
    if (IntuitionBase == 0) {
        IntuitionBase = (struct IntuitionBase *)
            OpenLibrary((CONST_STRPTR)"intuition.library", 36L);
        if (IntuitionBase == 0) {
            tg_wb_drop_diag = "intuition.library open failed";
            return;
        }
        tg_wb_drop_opened_int = 1;
    }
    /* InfoData must be longword-aligned for the packet: AllocMem it. */
    id = (struct InfoData *)AllocMem(sizeof(struct InfoData),
                                     MEMF_PUBLIC | MEMF_CLEAR);
    if (id == 0) {
        tg_wb_drop_diag = "no memory";
        tg_wb_drop_disarm();
        return;
    }
    win = 0;
    if (DoPkt(fh->fh_Type, ACTION_DISK_INFO, (LONG)MKBADDR(id),
              0, 0, 0, 0)) {
        /* the handler stuffs a RAW window pointer into the BPTR-typed
           field: plain cast, no BADDR (it would shift it into garbage) */
        win = (struct Window *)id->id_VolumeNode;
    }
    FreeMem(id, sizeof(struct InfoData));
    if (tg_wb_window_is_live(win)) {
        tg_wb_drop_diag = "ready (handler window)";
    } else {
        win = tg_wb_find_window_by_title("Telegram Amiga TUI");
        tg_wb_drop_diag = "ready (title match)";
    }
    if (win == 0) {
        tg_wb_drop_diag = "console window not found";
        tg_wb_drop_disarm();
        return;
    }
    if (WorkbenchBase == 0) {
        WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 36L);
        if (WorkbenchBase == 0) {
            tg_wb_drop_diag = "workbench.library open failed";
            tg_wb_drop_disarm();
            return;
        }
        tg_wb_drop_opened_wb = 1;
    }
    tg_wb_app_port = CreateMsgPort();
    if (tg_wb_app_port == 0) {
        tg_wb_drop_diag = "message port failed";
        tg_wb_drop_disarm();
        return;
    }
    tg_wb_app_win = AddAppWindowA(0UL, 0UL, win, tg_wb_app_port, 0);
    if (tg_wb_app_win == 0) {
        tg_wb_drop_diag = "AddAppWindow failed";
        tg_wb_drop_disarm();
        return;
    }
    /* NO AppIcon on OS3: field test showed drops land fine on the console
       WINDOW here (the classic con-handler does not own them), so the icon
       is redundant clutter. OS4 keeps it -- there the window lane is taken
       by the system and the icon is the only route. */
}

int tg_platform_console_drop_poll(char *out, unsigned long out_size)
{
    struct AppMessage *am;
    int got = 0;

    if (out == 0 || out_size == 0UL) {
        return 0;
    }
    out[0] = '\0';
    if (tg_wb_app_port == 0) {
        return 0;
    }
    ++tg_wb_drop_polls;
    while ((am = (struct AppMessage *)GetMsg(tg_wb_app_port)) != 0) {
        ++tg_wb_drop_msgs;
        if (!got && am->am_NumArgs > 0 && am->am_ArgList != 0) {
            BPTR lock = am->am_ArgList[0].wa_Lock;
            const char *name = (const char *)am->am_ArgList[0].wa_Name;

            out[0] = '\0';
            if (lock != 0 &&
                NameFromLock(lock, (STRPTR)out, (LONG)out_size) != 0) {
                if (name != 0 && name[0] != '\0') {
                    AddPart((STRPTR)out, (CONST_STRPTR)name, (ULONG)out_size);
                }
                got = 1;
            } else if (name != 0 && name[0] != '\0') {
                strncpy(out, name, out_size - 1UL);
                out[out_size - 1UL] = '\0';
                got = 1;
            }
        }
        ReplyMsg((struct Message *)am);
    }
    return got;
}

const char *tg_platform_console_drop_diag(void)
{
    static char diag_buf[96];

    sprintf(diag_buf, "%s, polls %lu, drops %lu", tg_wb_drop_diag,
            tg_wb_drop_polls, tg_wb_drop_msgs);
    return diag_buf;
}


/* Clickable links (0.0.8): hand the URL to the system's OpenURL/URLOpen
   shell command, synchronously with NIL: I/O (System() does not close the
   handles for us on the sync path). A missing command fails fast and the
   GUI falls back to copying the URL to the clipboard. */
int tg_platform_open_url(const char *url)
{
    char cmd[320];
    BPTR nil_in;
    BPTR nil_out;
    int rc = -1;
    const char *p;

    if (url == 0 || url[0] == '\0' || strlen(url) > 280) {
        return -1;
    }
    for (p = url; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\n' || *p == '\r' || *p == '*') {
            return -1; /* would break AmigaDOS quoting */
        }
    }
    nil_in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    nil_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (nil_in == 0 || nil_out == 0) {
        if (nil_in != 0) {
            Close(nil_in);
        }
        if (nil_out != 0) {
            Close(nil_out);
        }
        return -1;
    }
    if (rc != 0) {
        sprintf(cmd, "OpenURL \"%s\"", url);
        rc = (int)SystemTags((STRPTR)cmd, SYS_Input, (ULONG)nil_in,
                             SYS_Output, (ULONG)nil_out, TAG_DONE);
    }
    if (rc != 0) {
        sprintf(cmd, "URLOpen \"%s\"", url);
        rc = (int)SystemTags((STRPTR)cmd, SYS_Input, (ULONG)nil_in,
                             SYS_Output, (ULONG)nil_out, TAG_DONE);
    }
    Close(nil_in);
    Close(nil_out);
    return rc;
}


/* --- GUI drag-and-drop (0.0.8 punto 1e): same machinery, our own window ---
   The console arm above has to HUNT for the CON: window; the GUI hands us
   its Intuition window directly, so this is just port + AddAppWindow on the
   shared statics (single owner process-wide: TUI console drop or GUI). */
int tg_platform_gui_drop_arm(void *window)
{
    if (window == 0) {
        return -1;
    }
    if (tg_wb_app_port != 0 || tg_wb_app_win != 0) {
        return 0; /* already armed */
    }
    if (WorkbenchBase == 0) {
        WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 36L);
        if (WorkbenchBase == 0) {
            tg_wb_drop_diag = "workbench.library open failed";
            return -1;
        }
        tg_wb_drop_opened_wb = 1;
    }
    tg_wb_app_port = CreateMsgPort();
    if (tg_wb_app_port == 0) {
        tg_wb_drop_diag = "message port failed";
        return -1;
    }
    tg_wb_app_win = AddAppWindowA(0UL, 0UL, (struct Window *)window,
                                  tg_wb_app_port, 0);
    if (tg_wb_app_win == 0) {
        tg_wb_drop_diag = "AddAppWindow failed";
        tg_wb_drop_disarm();
        return -1;
    }
    tg_wb_drop_diag = "ready (GUI window)";
    return 0;
}

void tg_platform_gui_drop_disarm(void)
{
    tg_wb_drop_disarm();
}

unsigned long tg_platform_gui_drop_sigmask(void)
{
    return (tg_wb_app_port != 0) ? (1UL << tg_wb_app_port->mp_SigBit) : 0UL;
}
