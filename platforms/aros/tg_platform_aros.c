/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef TG_ENABLE_TLS
#define TG_ENABLE_TLS 0
#endif

#if defined(__AROS__)
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>

struct Library *SocketBase = 0;
/* NB: CloseSocket() is already available as a macro from the AROS bsdsocket
   defines (defines/bsdsocket.h, via SocketBase); we use it instead of close()
   to release a failed-connect socket -- close() leaves it in AROSTCP's list and
   the later CloseLibrary re-closes it, GURUing (soclose -> bsd_free). */
#else
#include <termios.h>
#endif

#if TG_ENABLE_TLS
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#endif

#include <time.h>

#if defined(__AROS__) || defined(__amigaos4__) || defined(__MORPHOS__) || defined(__amigaos3__) || defined(__m68k__)
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/icon.h>
#if defined(__AROS__)
#include <proto/wb.h>
#endif
#endif
#include "tg_platform.h"
#include "tg_mtproto_crypto.h"

#if defined(__AROS__)
struct tg_aros_stack_context {
    tg_platform_entry_fn entry;
    int argc;
    char **argv;
};

static IPTR tg_aros_stack_entry(IPTR context_value)
{
    struct tg_aros_stack_context *ctx;

    ctx = (struct tg_aros_stack_context *)context_value;
    return (IPTR)ctx->entry(ctx->argc, ctx->argv);
}
#endif

const char *tg_platform_name(void)
{
    return "AROS";
}

const char *tg_platform_default_data_dir(void)
{
    return "PROGDIR:";
}

int tg_platform_run_with_safe_stack(tg_platform_entry_fn entry,
                                    int argc, char **argv)
{
#if defined(__AROS__)
    struct tg_aros_stack_context context;
    struct Task *task;
    unsigned long current_size;
    APTR stack_memory;
    struct StackSwapStruct stack;
    struct StackSwapArgs args;
    IPTR result;

    task = FindTask(0);
    if (task == 0 || task->tc_SPUpper == 0 || task->tc_SPLower == 0) {
        return entry(argc, argv);
    }
    current_size = (unsigned long)((UBYTE *)task->tc_SPUpper -
                                   (UBYTE *)task->tc_SPLower);
    if (current_size >= TG_PLATFORM_SAFE_STACK_MIN) {
        return entry(argc, argv);
    }

    stack_memory = AllocVec(TG_PLATFORM_SAFE_STACK_SIZE,
                            MEMF_PUBLIC | MEMF_CLEAR);
    if (stack_memory == 0) {
        fprintf(stderr,
                "Telegram Amiga: not enough memory for the required 1 MiB stack.\n");
        return 20;
    }

    memset(&stack, 0, sizeof(stack));
    memset(&args, 0, sizeof(args));
    context.entry = entry;
    context.argc = argc;
    context.argv = argv;
    stack.stk_Lower = stack_memory;
    stack.stk_Upper = (UBYTE *)stack_memory + TG_PLATFORM_SAFE_STACK_SIZE;
    stack.stk_Pointer = (UBYTE *)stack.stk_Upper - sizeof(IPTR);
    args.Args[0] = (IPTR)&context;
    fprintf(stderr, "stack: swapped %lu -> %lu\n", current_size,
            (unsigned long)TG_PLATFORM_SAFE_STACK_SIZE);
    result = NewStackSwap(&stack, (LONG_FUNC)tg_aros_stack_entry, &args);
    FreeVec(stack_memory);
    return (int)result;
#else
    return entry(argc, argv);
#endif
}

unsigned long tg_platform_local_epoch(void)
{
#if defined(__AROS__)
    struct DateStamp ds;

    /* Raw LOCAL Amiga wall clock (the Workbench clock value), days/minutes/ticks
       since 1978-01-01, no timezone/DST applied -- used to anchor message-time
       display on the system clock rather than C time(). 252460800 = seconds from
       1970-01-01 to the 1978-01-01 Amiga epoch. */
    DateStamp(&ds);
    return (unsigned long)ds.ds_Days * 86400UL
         + (unsigned long)ds.ds_Minute * 60UL
         + (unsigned long)ds.ds_Tick / 50UL
         + 252460800UL;
#else
    /* Host build (this file backs the host target): time() already tracks the
       host's local clock and there is no Amiga DateStamp, so the skew is zero
       and message-time display is unchanged. */
    return (unsigned long)time(0);
#endif
}

#if defined(__AROS__)

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
#else
int tg_platform_wb_tui_mode(char **argv)
{
    (void)argv;
    return -1; /* host build: no Workbench icons */
}
#endif

void tg_platform_workbench_init(void)
{
#if defined(__AROS__)
    /* Workbench start: anchor the CWD to the binary's drawer so the relative
       data files resolve. The lock is process-lifetime (freed at exit). */
    BPTR progdir = Lock((CONST_STRPTR)"PROGDIR:", SHARED_LOCK);
    if (progdir != 0) {
        CurrentDir(progdir);
    }
#endif
}

#if defined(__AROS__)
static void tg_aros_close_socket_library(void);
#endif

void tg_platform_shutdown(void)
{
#if defined(__AROS__)
    /* bsdsocket.library is opened once (first connect) and owned process-wide:
       closing it per-connection zeroed the shared SocketBase under live
       connections (GUI keeps several open), and the next send() was an LVO
       call through a NULL base -> the x86_64 relaunch bus-fault. */
    tg_aros_close_socket_library();
#endif
}

void tg_platform_log(const char *level, const char *message)
{
    printf("[aros:%s] %s\n", level, message);
}

void tg_platform_debug(const char *message)
{
    (void)message; /* no dedicated kernel-debug channel used here */
}

void tg_platform_sleep_seconds(unsigned long seconds)
{
    if (seconds > 0) {
        sleep(seconds);
    }
}

int tg_platform_stdin_readable(unsigned long timeout_seconds)
{
#if defined(__AROS__)
    unsigned long long timeout_microseconds;

    timeout_microseconds = (unsigned long long)timeout_seconds * 1000000ULL;
    if (timeout_microseconds > 2147000000ULL) {
        timeout_microseconds = 2147000000ULL;
    }
    return WaitForChar(Input(), (long)timeout_microseconds) != 0;
#else
    fd_set read_fds;
    struct timeval timeout;
    int rc;

    FD_ZERO(&read_fds);
    FD_SET(0, &read_fds);
    timeout.tv_sec = (long)timeout_seconds;
    timeout.tv_usec = 0;
    rc = select(1, &read_fds, 0, 0, &timeout);
    return rc > 0 && FD_ISSET(0, &read_fds);
#endif
}

/*
 * Keystroke-timing entropy: every byte read from the console folds its
 * arrival time into a small ring at O(1) cost (no hashing on the input
 * path); the DRBG absorbs the ring on every generate. Human typing right
 * before the MTProto auth-key DH is exactly the entropy a virtualised TSC
 * lacks, and it was being thrown away.
 */
static unsigned long tg_aros_timebase(void);

static unsigned long tg_aros_key_ring[16];
static unsigned long tg_aros_key_ring_pos = 0;

static void tg_aros_note_input_event(int ch)
{
    unsigned long v = tg_aros_timebase() ^
                      ((unsigned long)(unsigned char)ch << 24) ^
                      (tg_aros_key_ring_pos * 2654435761UL);
    tg_aros_key_ring[tg_aros_key_ring_pos & 15UL] ^=
        (v << (tg_aros_key_ring_pos & 7UL)) ^ (v >> 5);
    ++tg_aros_key_ring_pos;
}

/* Public GUI hook: same ring, full event words (see tg_platform.h). */
void tg_platform_note_input_event(unsigned long a, unsigned long b)
{
    unsigned long v = tg_aros_timebase() ^ a ^ (b << 13) ^ (b >> 7) ^
                      (tg_aros_key_ring_pos * 2654435761UL);
    tg_aros_key_ring[tg_aros_key_ring_pos & 15UL] ^=
        (v << (tg_aros_key_ring_pos & 7UL)) ^ (v >> 5);
    ++tg_aros_key_ring_pos;
}

int tg_platform_stdin_read_char(unsigned long timeout_seconds, char *out_char)
{
#if defined(__AROS__)
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
    tg_aros_note_input_event((int)ch);
    *out_char = ch;
    return 1;
#else
    fd_set read_fds;
    struct timeval timeout;
    char ch;
    int rc;
    ssize_t got;

    if (out_char == 0) {
        return -1;
    }
    FD_ZERO(&read_fds);
    FD_SET(0, &read_fds);
    timeout.tv_sec = (long)timeout_seconds;
    timeout.tv_usec = 0;
    rc = select(1, &read_fds, 0, 0, &timeout);
    if (rc <= 0 || !FD_ISSET(0, &read_fds)) {
        return 0;
    }
    got = read(0, &ch, 1);
    if (got <= 0) {
        return -1;
    }
    tg_aros_note_input_event((int)ch);
    *out_char = ch;
    return 1;
#endif
}

int tg_platform_stdin_read_hidden_line(char *out, unsigned long out_size)
{
#if defined(__AROS__)
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
        tg_aros_note_input_event((int)ch);
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
    struct termios old_term;
    struct termios new_term;
    int have_term;
    unsigned long pos;
    int c;

    if (out == 0 || out_size == 0UL) {
        return -1;
    }
    out[0] = '\0';
    pos = 0UL;
    have_term = (tcgetattr(0, &old_term) == 0);
    if (have_term) {
        new_term = old_term;
        new_term.c_lflag &= ~(tcflag_t)ECHO;
        (void)tcsetattr(0, TCSANOW, &new_term);
    }
    for (;;) {
        c = getchar();
        if (c == EOF) {
            if (have_term) {
                (void)tcsetattr(0, TCSANOW, &old_term);
            }
            return (pos > 0UL) ? 0 : -1;
        }
        if (c == '\n' || c == '\r') {
            break;
        }
        if (pos + 1UL < out_size) {
            out[pos++] = (char)c;
        }
    }
    out[pos] = '\0';
    if (have_term) {
        (void)tcsetattr(0, TCSANOW, &old_term);
    }
    return 0;
#endif
}

int tg_platform_stdin_set_raw(int enabled)
{
#if defined(__AROS__)
    if (SetMode(Input(), enabled ? 1 : 0)) {
        return 0;
    }
    return -1;
#else
    static struct termios saved;
    static int saved_valid = 0;
    struct termios raw;

    if (enabled) {
        if (tcgetattr(0, &saved) != 0) {
            return -1;
        }
        saved_valid = 1;
        raw = saved;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &raw) != 0) {
            return -1;
        }
        return 0;
    }
    if (saved_valid) {
        (void)tcsetattr(0, TCSANOW, &saved);
        saved_valid = 0;
    }
    return 0;
#endif
}

/*
 * In-tree CSPRNG fallback for AROS (SHA-256 Hash-DRBG).
 *
 * Real AROS has no /dev/urandom and this MTProto build links no TLS, so the
 * POSIX random path returns nothing and login fails with "secure-rng-
 * unavailable". Mirror the AmigaOS 3/4 backends: gather local entropy (x86 TSC
 * jitter, wall clock, run-varying addresses, a per-call counter) and run it
 * through SHA-256 in a Hash-DRBG. Only used when /dev/urandom is absent, so the
 * macOS host build keeps using the real kernel CSPRNG.
 */
static unsigned char tg_aros_drbg_state[TG_MTPROTO_SHA256_LENGTH];
static int tg_aros_drbg_ready = 0;

static unsigned long tg_aros_timebase(void)
{
    unsigned long tb = 0;
#if defined(__i386__) || defined(__x86_64__)
    unsigned int lo = 0;
    unsigned int hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    tb = (unsigned long)lo ^ ((unsigned long)hi << 13);
#else
    struct timeval tv;
    gettimeofday(&tv, 0);
    tb = (unsigned long)tv.tv_usec ^ ((unsigned long)tv.tv_sec << 8);
#endif
    return tb;
}

static unsigned long tg_aros_entropy_gather(unsigned char *buf,
                                            unsigned long cap)
{
    unsigned long n = 0;
    unsigned long i;
    struct timeval tv;
    time_t now;
    void *p;

    now = time(0);
    if (n + sizeof(now) <= cap) {
        memcpy(buf + n, &now, sizeof(now));
        n += sizeof(now);
    }
    for (i = 0; i < 96UL && n + sizeof(unsigned long) <= cap; ++i) {
        unsigned long tb = tg_aros_timebase();
        volatile unsigned long spin;
        unsigned long k;
        memcpy(buf + n, &tb, sizeof(tb));
        n += sizeof(tb);
        spin = tb;
        for (k = 0; k < (tb & 0x3fUL); ++k) {
            spin = (spin * 2654435761UL) + k;
        }
        (void)spin;
        if (n + sizeof(tv) <= cap) {
            gettimeofday(&tv, 0);
            memcpy(buf + n, &tv, sizeof(tv));
            n += sizeof(tv);
        }
    }
    p = (void *)&tv;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    p = (void *)buf;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    p = (void *)tg_aros_drbg_state;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    p = (void *)&n;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    return n;
}

static void tg_aros_drbg_generate(unsigned char *out, unsigned long n);

static FILE *tg_aros_open_seed_file(const char *mode)
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
 * reproducible VM boot. The file is mixed into the pool at seed time and
 * immediately overwritten with fresh DRBG output, so yesterday's file
 * never predicts today's state.
 */
static void tg_aros_drbg_seed(void)
{
    unsigned char pool[1024];
    unsigned long len = tg_aros_entropy_gather(pool, sizeof(pool));
    {
        FILE *seed_file = tg_aros_open_seed_file("rb");
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
    tg_mtproto_sha256(pool, len, tg_aros_drbg_state);
    tg_aros_drbg_ready = 1;
    {
        unsigned char fresh[64];
        FILE *seed_file;
        tg_aros_drbg_generate(fresh, sizeof(fresh));
        seed_file = tg_aros_open_seed_file("wb");
        if (seed_file != 0) {
            fwrite(fresh, 1U, sizeof(fresh), seed_file);
            fclose(seed_file);
        }
        memset(fresh, 0, sizeof(fresh));
    }
}

static void tg_aros_drbg_generate(unsigned char *out, unsigned long n)
{
    static unsigned long calls = 0;
    unsigned char key[TG_MTPROTO_SHA256_LENGTH];
    unsigned char block[TG_MTPROTO_SHA256_LENGTH];
    unsigned long off;
    unsigned long ctr;

    if (!tg_aros_drbg_ready) {
        tg_aros_drbg_seed();
    }
    {
        unsigned char work[TG_MTPROTO_SHA256_LENGTH + 32 +
                           sizeof(tg_aros_key_ring) + sizeof(unsigned long)];
        unsigned long wn = TG_MTPROTO_SHA256_LENGTH;
        struct timeval tv;
        unsigned long tb = tg_aros_timebase();
        memcpy(work, tg_aros_drbg_state, TG_MTPROTO_SHA256_LENGTH);
        gettimeofday(&tv, 0);
        ++calls;
        memcpy(work + wn, &tv, sizeof(tv)); wn += sizeof(tv);
        memcpy(work + wn, &tb, sizeof(tb)); wn += sizeof(tb);
        memcpy(work + wn, &calls, sizeof(calls)); wn += sizeof(calls);
        /* Keystroke-timing ring: human input collected since the last
           generate (cheap on the input path, absorbed here). */
        memcpy(work + wn, tg_aros_key_ring, sizeof(tg_aros_key_ring));
        wn += sizeof(tg_aros_key_ring);
        memcpy(work + wn, &tg_aros_key_ring_pos,
               sizeof(tg_aros_key_ring_pos));
        wn += sizeof(tg_aros_key_ring_pos);
        tg_mtproto_sha256(work, wn, key);
    }
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
        tg_mtproto_sha256(rb, sizeof(rb), tg_aros_drbg_state);
    }
}

int tg_platform_random_bytes(unsigned char *bytes, unsigned long byte_count)
{
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
    if (fd >= 0) {
        offset = 0;
        while (offset < byte_count) {
            got = read(fd, bytes + offset, byte_count - offset);
            if (got <= 0) {
                break;
            }
            offset += (unsigned long)got;
        }
        close(fd);
        if (offset >= byte_count) {
            return 1;
        }
    }
#if TG_ENABLE_TLS
    if (RAND_bytes(bytes, (int)byte_count) == 1) {
        return 1;
    }
#endif
    /* No /dev/urandom on real AROS: use the in-tree Hash-DRBG. */
    tg_aros_drbg_generate(bytes, byte_count);
    return 1;
}

static void tg_platform_set_error(char *error_buffer, unsigned long error_buffer_size,
                                  const char *message)
{
    if (error_buffer != 0 && error_buffer_size > 0) {
        strncpy(error_buffer, message, error_buffer_size - 1);
        error_buffer[error_buffer_size - 1] = '\0';
    }
}

#if defined(__AROS__)
static int tg_aros_open_socket_library(void)
{
    if (SocketBase != 0) {
        return 1;
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 3);
    return SocketBase != 0;
}

static void tg_aros_close_socket_library(void)
{
    if (SocketBase != 0) {
        CloseLibrary(SocketBase);
        SocketBase = 0;
    }
}
#endif

#if !defined(__AROS__)
static tg_net_status tg_platform_connect_socket(int sock, struct sockaddr_in *address,
                                                char *error_buffer,
                                                unsigned long error_buffer_size)
{
    unsigned long timeout_seconds;
    int flags;
    int rc;
    int socket_error;
    socklen_t socket_error_size;
    fd_set write_fds;
    struct timeval timeout;

    timeout_seconds = tg_net_connect_timeout_seconds();
    if (timeout_seconds == 0) {
        rc = connect(sock, (struct sockaddr *)address, sizeof(*address));
        if (rc == 0) {
            return TG_NET_OK;
        }
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        return TG_NET_CONNECT_FAILED;
    }

    flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        rc = connect(sock, (struct sockaddr *)address, sizeof(*address));
        if (rc == 0) {
            return TG_NET_OK;
        }
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        return TG_NET_CONNECT_FAILED;
    }

    rc = connect(sock, (struct sockaddr *)address, sizeof(*address));
    if (rc == 0) {
        (void)fcntl(sock, F_SETFL, flags);
        return TG_NET_OK;
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        (void)fcntl(sock, F_SETFL, flags);
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        return TG_NET_CONNECT_FAILED;
    }

    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeout.tv_sec = (long)timeout_seconds;
    timeout.tv_usec = 0;

    rc = select(sock + 1, 0, &write_fds, 0, &timeout);
    if (rc <= 0) {
        (void)fcntl(sock, F_SETFL, flags);
        if (rc == 0) {
            tg_platform_set_error(error_buffer, error_buffer_size,
                                  "socket connect timed out");
        } else {
            tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        }
        return TG_NET_CONNECT_FAILED;
    }

    socket_error = 0;
    socket_error_size = sizeof(socket_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error,
                   &socket_error_size) != 0) {
        (void)fcntl(sock, F_SETFL, flags);
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        return TG_NET_CONNECT_FAILED;
    }
    if (socket_error != 0) {
        (void)fcntl(sock, F_SETFL, flags);
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(socket_error));
        return TG_NET_CONNECT_FAILED;
    }

    (void)fcntl(sock, F_SETFL, flags);
    return TG_NET_OK;
}
#endif

tg_net_status tg_platform_tcp_connect(tg_net_connection *connection, const char *host,
                                      const char *port, char *error_buffer,
                                      unsigned long error_buffer_size)
{
    struct hostent *host_entry;
    struct sockaddr_in address;
    long port_number;
    int rc;
    int sock;

    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }

    port_number = strtol(port, 0, 10);
    if (port_number <= 0 || port_number > 65535) {
        return TG_NET_INVALID_ARGUMENT;
    }

#if defined(__AROS__)
    if (!tg_aros_open_socket_library()) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "cannot open bsdsocket.library");
        return TG_NET_CONNECT_FAILED;
    }
#endif

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port_number);
    {
        /* Telegram DC addresses are numeric IPv4 literals. Parse them with
           inet_addr() and SKIP gethostbyname(): AROS/AROSTCP's gethostbyname()
           on x86_64 corrupts the socket heap for an IP literal (a freed block
           ends up holding the IP string -> GURU in soclose/bsd_free when the
           socket is later closed). Only fall back to DNS for real host names. */
        unsigned long numeric = (unsigned long)(unsigned int)inet_addr(host);
        if (numeric != 0xFFFFFFFFUL) {
            address.sin_addr.s_addr = (unsigned int)numeric;
        } else {
            host_entry = gethostbyname(host);
            if (host_entry == 0 || host_entry->h_addr_list == 0 ||
                host_entry->h_addr_list[0] == 0) {
                tg_platform_set_error(error_buffer, error_buffer_size,
                                      "host lookup failed");
                return TG_NET_RESOLVE_FAILED;
            }
            memcpy(&address.sin_addr, host_entry->h_addr_list[0],
                   sizeof(address.sin_addr));
        }
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        return TG_NET_CONNECT_FAILED;
    }

#if defined(__AROS__)
    rc = connect(sock, (struct sockaddr *)&address, sizeof(address));
    if (rc == 0) {
#else
    (void)rc;
    if (tg_platform_connect_socket(sock, &address, error_buffer,
                                   error_buffer_size) == TG_NET_OK) {
#endif
        connection->platform_handle = sock;
        connection->is_open = 1;
        return TG_NET_OK;
    }

#if defined(__AROS__)
    /* CloseSocket(), not close(): close() leaves a failed-connect socket in
       AROSTCP's list and the CloseLibrary below would re-close it and crash. */
    CloseSocket(sock);
    tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
#else
    close(sock);
#endif
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
#if defined(__AROS__)
    if (SocketBase == 0) {
        /* A bsdsocket macro through a NULL base is an LVO call through 0:
           fail cleanly instead (can only happen on a connection misuse). */
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "socket library not open");
        return TG_NET_SEND_FAILED;
    }
#endif
    /* Bound the blocking send() with a writability wait, mirroring the recv
       timeout: a wedged link (QEMU slirp stalls on sustained uploads) left
       send() blocked FOREVER -- upload frozen at N%, no error, cancel dead.
       A stalled socket now surfaces as a timeout, so the per-part retry (on a
       fresh connection) or a clean abort takes over. */
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
        src = select((int)connection->platform_handle + 1, 0, &write_fds, 0,
                     &timeout);
        if (src <= 0 || !FD_ISSET((int)connection->platform_handle,
                                  &write_fds)) {
            tg_platform_set_error(error_buffer, error_buffer_size,
                                  src == 0 ? "socket send timed out"
                                           : strerror(errno));
            return src == 0 ? TG_NET_TIMEOUT : TG_NET_SEND_FAILED;
        }
    }

    rc = send((int)connection->platform_handle, data, byte_count, 0);
    if (rc < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
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
    unsigned long timeout_seconds;
    fd_set read_fds;
    struct timeval timeout;
    long rc;

    if (bytes_received != 0) {
        *bytes_received = 0;
    }
    if (error_buffer != 0 && error_buffer_size > 0) {
        error_buffer[0] = '\0';
    }
#if defined(__AROS__)
    if (SocketBase == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "socket library not open");
        return TG_NET_RECV_FAILED;
    }
#endif

    /* Bound the blocking recv() with a select() timeout, mirroring the MorphOS
       backend. Without this the encrypted-query receive loop in
       tg_mtproto_send_saved_query_on_context() can block forever inside recv()
       when the server goes quiet (e.g. a contacts.search from /add with no
       prompt reply): its 12s wall-clock budget is only checked between reads,
       so a stuck recv() never lets it fire and the chat hangs after /add. */
    timeout_seconds = tg_net_connect_timeout_seconds();
    if (timeout_seconds == 0UL) {
        timeout_seconds = 30UL;
    }
    FD_ZERO(&read_fds);
    FD_SET((int)connection->platform_handle, &read_fds);
    timeout.tv_sec = (long)timeout_seconds;
    timeout.tv_usec = 0;
    rc = select((int)connection->platform_handle + 1, &read_fds, 0, 0,
                &timeout);
    if (rc <= 0 || !FD_ISSET((int)connection->platform_handle, &read_fds)) {
        if (rc == 0) {
            tg_platform_set_error(error_buffer, error_buffer_size,
                                  "socket receive timed out");
        } else {
            tg_platform_set_error(error_buffer, error_buffer_size,
                                  strerror(errno));
        }
        return rc == 0 ? TG_NET_TIMEOUT : TG_NET_RECV_FAILED;
    }

    rc = recv((int)connection->platform_handle, buffer, buffer_size, 0);
    if (rc < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
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
#if defined(__AROS__)
    if (SocketBase == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "socket library not open");
        return -1;
    }
#endif
    FD_ZERO(&read_fds);
    FD_SET((int)connection->platform_handle, &read_fds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    rc = select((int)connection->platform_handle + 1, &read_fds, 0, 0,
                &timeout);
    if (rc < 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, strerror(errno));
        return -1;
    }
    return rc > 0 &&
           FD_ISSET((int)connection->platform_handle, &read_fds) ? 1 : 0;
}

void tg_platform_tcp_close(tg_net_connection *connection)
{
    if (connection != 0 && connection->is_open) {
#if defined(__AROS__)
        /* CloseSocket(), not close(): close() leaves the socket in AROSTCP's
           list, and the CloseLibrary at shutdown would re-close it and corrupt
           the TLSF heap (same defect the failed-connect path already fixed). */
        CloseSocket((long)connection->platform_handle);
#else
        close((int)connection->platform_handle);
#endif
        connection->is_open = 0;
    }
    /* The socket library itself stays open until tg_platform_shutdown(): other
       connections may still be using the shared base. */
}

#if TG_ENABLE_TLS

static void tg_platform_set_ssl_error(char *error_buffer, unsigned long error_buffer_size)
{
    unsigned long error_code;
    const char *error_string;

    error_code = ERR_get_error();
    if (error_code == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, "TLS operation failed");
        return;
    }

    error_string = ERR_reason_error_string(error_code);
    if (error_string == 0) {
        error_string = "TLS operation failed";
    }
    tg_platform_set_error(error_buffer, error_buffer_size, error_string);
}

static tg_tls_status tg_aros_configure_certificate_validation(SSL_CTX *ctx,
                                                             SSL *ssl,
                                                             const char *host,
                                                             char *error_buffer,
                                                             unsigned long error_buffer_size)
{
    const char *ca_file;
    const char *ca_path;
    X509_VERIFY_PARAM *verify_param;

    if (!tg_tls_certificate_validation_enabled()) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);
        return TG_TLS_OK;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, 0);
    ca_file = tg_tls_certificate_ca_file();
    ca_path = tg_tls_certificate_ca_path();
    if (ca_file != 0 || ca_path != 0) {
        if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_path) != 1) {
            tg_platform_set_ssl_error(error_buffer, error_buffer_size);
            return TG_TLS_VERIFY_FAILED;
        }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not load default CA paths");
        return TG_TLS_VERIFY_FAILED;
    }

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    verify_param = SSL_get0_param(ssl);
    if (verify_param == 0 ||
        X509_VERIFY_PARAM_set1_host(verify_param, host, 0) != 1) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not enable hostname verification");
        return TG_TLS_VERIFY_FAILED;
    }
#else
    (void)verify_param;
    tg_platform_set_error(error_buffer, error_buffer_size,
                          "hostname verification is not supported by OpenSSL");
    return TG_TLS_VERIFY_FAILED;
#endif

    return TG_TLS_OK;
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

    OPENSSL_init_ssl(0, 0);

    local_net_status = tg_net_connect(&connection->tcp, host, port,
                                      error_buffer, error_buffer_size);
    if (local_net_status != TG_NET_OK) {
        if (net_status != 0) {
            *net_status = local_net_status;
        }
        return TG_TLS_NET_ERROR;
    }

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size);
        tg_net_close(&connection->tcp);
        return TG_TLS_HANDSHAKE_FAILED;
    }

#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    ssl = SSL_new(ctx);
    if (ssl == 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        return TG_TLS_HANDSHAKE_FAILED;
    }

    SSL_set_fd(ssl, (int)connection->tcp.platform_handle);
    SSL_set_tlsext_host_name(ssl, host);
    verify_status = tg_aros_configure_certificate_validation(
        ctx, ssl, host, error_buffer, error_buffer_size);
    if (verify_status != TG_TLS_OK) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        return verify_status;
    }

    if (SSL_connect(ssl) != 1) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
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

    rc = SSL_write((SSL *)connection->platform_session, data, (int)byte_count);
    if (rc <= 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size);
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

    rc = SSL_read((SSL *)connection->platform_session, buffer, (int)buffer_size);
    if (rc > 0) {
        if (bytes_received != 0) {
            *bytes_received = (unsigned long)rc;
        }
        return TG_TLS_OK;
    }

    ssl_error = SSL_get_error((SSL *)connection->platform_session, rc);
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return TG_TLS_CLOSED;
    }

    tg_platform_set_ssl_error(error_buffer, error_buffer_size);
    return TG_TLS_RECV_FAILED;
}

void tg_platform_tls_close(tg_tls_connection *connection)
{
    if (connection == 0) {
        return;
    }
    if (connection->platform_session != 0) {
        SSL_shutdown((SSL *)connection->platform_session);
        SSL_free((SSL *)connection->platform_session);
        connection->platform_session = 0;
    }
    if (connection->platform_context != 0) {
        SSL_CTX_free((SSL_CTX *)connection->platform_context);
        connection->platform_context = 0;
    }
    tg_net_close(&connection->tcp);
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
#if defined(__AROS__)
    /* Peek without clearing: the break stays pending for outer loops. */
    return (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) != 0L;
#else
    return 0;
#endif
}

#if defined(__AROS__)
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
#if defined(__AROS__)
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
#if defined(__AROS__)
    /* The screen flash is the Amiga-native notification; a BEL byte lets
       console handlers improvise (one icon console draws it as a glyph). */
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
#if defined(__AROS__)

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
#else
void tg_platform_ensure_drawer_icon(const char *drawer)
{
    (void)drawer; /* host build: no Workbench icons */
}
#endif
#if defined(__AROS__)

static void tg_wb_drop_arm(void);
static void tg_wb_drop_disarm(void);

static BPTR tg_wb_tui_con = 0;
static BPTR tg_wb_tui_old_in = 0;
static BPTR tg_wb_tui_old_out = 0;
static struct MsgPort *tg_wb_tui_old_ct = 0;

int tg_platform_workbench_tui_console(void)
{
    BPTR con;

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
    /* Farewell pause under OUR control (the plain ROM 3.1 con-handler never
       dismissed a WAIT window; the same deterministic scheme runs on every
       lane): one keypress -- or a close-click EOF where the handler sends
       one -- ends the pause, then every handle goes and the window dies
       with the last Close(). */
    SetMode(Input(), 0);
    {
        char ch;

        (void)Read(Input(), &ch, 1);
    }
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

extern struct Library *WorkbenchBase; /* owned by core/tg_gui_window.c */
extern struct Library *IconBase;

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
    /* SIPTR, NOT LONG: on AROS x86_64 LONG is 32-bit while pointers are
       64-bit -- a LONG cast truncates the InfoData address and the handler
       would write through garbage (found by review before it shipped). */
    if (DoPkt(fh->fh_Type, ACTION_DISK_INFO, (SIPTR)MKBADDR(id),
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
    /* NO AppIcon on AROS: field test showed Wanderer displays it but never
       delivers drops to it (its AppIcon support is incomplete), while drops
       on the console WINDOW work -- the opposite of OS4. An inert icon is
       just confusion, so the window lane is the AROS path. */
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


#else
int tg_platform_workbench_tui_console(void)
{
    return 0; /* host build: no Workbench console */
}

void tg_platform_workbench_tui_console_close(void)
{
    /* host build: nothing was opened */
}
#endif

#if !defined(__AROS__)
/* Host build: no Workbench, no console drops. */
int tg_platform_console_drop_poll(char *out, unsigned long out_size)
{
    if (out != 0 && out_size > 0UL) {
        out[0] = '\0';
    }
    return 0;
}

const char *tg_platform_console_drop_diag(void)
{
    return "unsupported on this platform";
}
#endif

#if defined(__AROS__)

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
        rc = (int)SystemTags((STRPTR)cmd, SYS_Input, (IPTR)nil_in,
                             SYS_Output, (IPTR)nil_out, TAG_DONE);
    }
    if (rc != 0) {
        sprintf(cmd, "URLOpen \"%s\"", url);
        rc = (int)SystemTags((STRPTR)cmd, SYS_Input, (IPTR)nil_in,
                             SYS_Output, (IPTR)nil_out, TAG_DONE);
    }
    Close(nil_in);
    Close(nil_out);
    return rc;
}
#else
/* Hosted test build: no system browser hook; the GUI falls back to the
   clipboard copy path. */
int tg_platform_open_url(const char *url)
{
    (void)url;
    return -1;
}
#endif

#if defined(__AROS__)

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
#else
/* Host build: no Workbench window to arm. */
int tg_platform_gui_drop_arm(void *window)
{
    (void)window;
    return -1;
}

void tg_platform_gui_drop_disarm(void)
{
}

unsigned long tg_platform_gui_drop_sigmask(void)
{
    return 0UL;
}
#endif
