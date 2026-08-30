/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#if !defined(__amigaos4__)
#include <fcntl.h>
#endif

#ifndef TG_AMIGAOS4_ENABLE_AMISSL
#define TG_AMIGAOS4_ENABLE_AMISSL 0
#endif

#if defined(__amigaos4__)
#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <devices/timer.h>
#include <interfaces/timer.h> /* struct TimerIFace only: proto/timer.h's
                                 __USE_INLINE__ macros would rewrite our
                                 explicit iface->ReadEClock() call */
#include <dos/dosextens.h>
#include <dos/dostags.h> /* SYS_Input/SYS_Output for the URL opener */
#include <exec/tasks.h>
#include <exec/exectags.h> /* ASOT_*/ASOIOR_* for AllocSysObject (PR #10) */
#include <proto/dos.h>
/* AmigaOS 4 renamed the classic DOS entries and keeps the old spellings here,
   Examine() and FIBF_EXECUTE among them. Including this beats redefining
   them locally, the same choice PR #12 made for the directory scan. */
#include <dos/obsolete.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/socket.h>
#endif

#if defined(__amigaos4__) && TG_AMIGAOS4_ENABLE_AMISSL
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
#endif
#if defined(__amigaos4__)
/* Types only, not proto/*.h: with __USE_INLINE__ the proto headers rewrite
   every call as GlobalIface->method, and we hold our own interface pointers
   (workbench + intuition are not auto-opened). */
#include <interfaces/wb.h>
#include <interfaces/intuition.h>
#include <intuition/screens.h>
#ifndef ACTION_DISK_INFO
#define ACTION_DISK_INFO 25
#endif
#endif
#include "tg_platform.h"
#include "tg_mtproto_crypto.h"

#if defined(__amigaos4__)
struct Library *SocketBase = 0;
struct SocketIFace *ISocket = 0;
#endif

#if defined(__amigaos4__) && TG_AMIGAOS4_ENABLE_AMISSL
struct Library *AmiSSLMasterBase = 0;
struct AmiSSLMasterIFace *IAmiSSLMaster = 0;
struct AmiSSLIFace *IAmiSSL = 0;

static int tg_amigaos4_amissl_initialized = 0;

static int tg_amigaos4_amissl_init(char *error_buffer,
                                   unsigned long error_buffer_size);
#endif

const char *tg_platform_name(void)
{
    return "AmigaOS 4.x";
}

const char *tg_platform_default_data_dir(void)
{
    return "PROGDIR:";
}

int tg_platform_run_with_safe_stack(tg_platform_entry_fn entry,
                                    int argc, char **argv)
{
#if defined(__amigaos4__)
    struct Task *task;
    unsigned long current_size;

    /* AmigaOS 4 officially honours the $STACK cookie before main(). Avoid a
       manual PPC StackSwap here: if a non-standard launcher ignores the
       cookie, fail visibly rather than entering the GUI on unsafe bounds. */
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

    /* Raw LOCAL Amiga wall clock (the Workbench clock value), days/minutes/ticks
       since 1978-01-01, no timezone/DST applied -- used to anchor message-time
       display on the system clock rather than C time(). 252460800 = seconds from
       1970-01-01 to the 1978-01-01 Amiga epoch. */
    DateStamp(&ds);
    return (unsigned long)ds.ds_Days * 86400UL
         + (unsigned long)ds.ds_Minute * 60UL
         + (unsigned long)ds.ds_Tick / 50UL
         + 252460800UL;
}

/* issue #9: read the launched icon's TUI_MODE tooltype on OS4. Uses the same
   local-IIcon alias trick as ensure_drawer_icon so the __USE_INLINE__ icon
   macros resolve to our opened interface; SetCurrentDir goes through the
   auto-provided IDOS. Returns 1 (TUI), 0 (GUI), -1 (no tooltype -> heuristic). */
int tg_platform_wb_tui_mode(char **argv)
{
#if defined(__amigaos4__)
    struct WBStartup *wb = (struct WBStartup *)argv;
    struct Library *IconBase;
    struct IconIFace *iicon;
    struct WBArg *a;
    int result = -1;

    if (wb == 0 || wb->sm_ArgList == 0 || wb->sm_NumArgs < 1) {
        return -1;
    }
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 44L);
    if (IconBase == 0) {
        return -1;
    }
    iicon = (struct IconIFace *)GetInterface(IconBase, "main", 1L, 0);
    if (iicon == 0) {
        CloseLibrary(IconBase);
        return -1;
    }
    {
        struct IconIFace *IIcon = iicon; (void)IIcon;
        struct DiskObject *dobj;

        a = &wb->sm_ArgList[0];
        if (a->wa_Name != 0 && a->wa_Name[0] != '\0') {
            BPTR olddir = 0;
            int have_dir = 0;

            if (a->wa_Lock != 0) {
                olddir = SetCurrentDir(a->wa_Lock);
                have_dir = 1;
            }
            dobj = GetDiskObject((STRPTR)a->wa_Name);
            if (dobj != 0) {
                STRPTR tt = FindToolType(dobj->do_ToolTypes,
                                         (CONST_STRPTR)"TUI_MODE");

                if (tt != 0) {
                    result = (MatchToolValue(tt, (CONST_STRPTR)"NO") ||
                              MatchToolValue(tt, (CONST_STRPTR)"FALSE") ||
                              MatchToolValue(tt, (CONST_STRPTR)"OFF")) ? 0 : 1;
                }
                FreeDiskObject(dobj);
            }
            if (have_dir) {
                SetCurrentDir(olddir);
            }
        }
    }
    DropInterface((struct Interface *)iicon);
    CloseLibrary(IconBase);
    return result;
#else
    (void)argv;
    return -1;
#endif
}

void tg_platform_workbench_init(void)
{
#if defined(__amigaos4__)
    /* Workbench start: anchor the CWD to the binary's drawer so the relative
       data files resolve. The lock is process-lifetime (freed at exit). */
    BPTR progdir = Lock((CONST_STRPTR)"PROGDIR:", SHARED_LOCK);
    if (progdir != 0) {
        /* AmigaOS 4.x renamed dos.library's CurrentDir() to SetCurrentDir();
           the classic name is not in the OS4 inline set (Lock() kept its name). */
        SetCurrentDir(progdir);
    }
#endif
}

void tg_platform_log(const char *level, const char *message)
{
    printf("[amigaos4:%s] %s\n", level, message);
}

void tg_platform_debug(const char *message)
{
    (void)message; /* no dedicated kernel-debug channel used here */
}

void tg_platform_sleep_seconds(unsigned long seconds)
{
#if defined(__amigaos4__)
    if (seconds > 0) {
        sleep(seconds);
    }
#else
    if (seconds > 0) {
        sleep((unsigned int)seconds);
    }
#endif
}

int tg_platform_stdin_readable(unsigned long timeout_seconds)
{
#if defined(__amigaos4__)
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
 * path); the fallback DRBG absorbs the ring on every generate. AmiSSL's
 * RAND_bytes stays the primary source -- this only strengthens the
 * fallback that runs when AmiSSL is unavailable.
 */
#if defined(__amigaos4__)
static unsigned long tg_os4_timebase(void);

static unsigned long tg_os4_key_ring[16];
static unsigned long tg_os4_key_ring_pos = 0;

static void tg_os4_note_input_event_words(unsigned long a, unsigned long b)
{
    unsigned long v = tg_os4_timebase() ^ a ^ (b << 13) ^ (b >> 7) ^
                      (tg_os4_key_ring_pos * 2654435761UL);
    tg_os4_key_ring[tg_os4_key_ring_pos & 15UL] ^=
        (v << (tg_os4_key_ring_pos & 7UL)) ^ (v >> 5);
    ++tg_os4_key_ring_pos;
}

static void tg_os4_note_input_event(int ch)
{
    unsigned long v = tg_os4_timebase() ^
                      ((unsigned long)(unsigned char)ch << 24) ^
                      (tg_os4_key_ring_pos * 2654435761UL);
    tg_os4_key_ring[tg_os4_key_ring_pos & 15UL] ^=
        (v << (tg_os4_key_ring_pos & 7UL)) ^ (v >> 5);
    ++tg_os4_key_ring_pos;
}
#endif

/* Public GUI hook: same ring, full event words (see tg_platform.h). */
void tg_platform_note_input_event(unsigned long a, unsigned long b)
{
#if defined(__amigaos4__)
    tg_os4_note_input_event_words(a, b);
#else
    (void)a;
    (void)b;
#endif
}

int tg_platform_stdin_read_char(unsigned long timeout_seconds, char *out_char)
{
#if defined(__amigaos4__)
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
    tg_os4_note_input_event((int)ch);
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
#if defined(__amigaos4__)
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
        tg_os4_note_input_event((int)ch);
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
#if defined(__amigaos4__)
    if (SetMode(Input(), enabled ? 1 : 0)) {
        return 0;
    }
    return -1;
#else
    (void)enabled;
    return -1;
#endif
}

#if defined(__amigaos4__)
/*
 * In-tree CSPRNG for AmigaOS 4.x (SHA-256 Hash-DRBG).
 *
 * AmiSSL's first RAND_bytes() call triggers OpenSSL provider initialisation
 * and DRBG (re)seeding, which is pathologically slow on OS4 / emulated PPC
 * (measured ~2 minutes before the first random byte is produced). MTProto on
 * OS4 uses the in-tree big-integer and SHA implementations for all of its
 * cryptography, so we avoid AmiSSL entirely for randomness: we gather local
 * entropy and run it through SHA-256 in a Hash-DRBG construction.
 *
 * Entropy sources: PPC time-base register (high resolution, jitter), wall
 * clock (gettimeofday + time), several run-varying addresses and a per-call
 * counter. Each call mixes fresh entropy into a working key, emits output as
 * SHA-256(key || block-counter), then ratchets the persistent state forward
 * (SHA-256(key || 0x01)) so prior output cannot be reconstructed.
 */
static unsigned char tg_os4_drbg_state[TG_MTPROTO_SHA256_LENGTH];
static int tg_os4_drbg_ready = 0;

/* Fine-grained tick for the entropy mix. The obvious mftb read is a TRAP on
   the AmigaOne A1222: its e500v2 core does not implement the classic mftb
   instruction (the time base is an SPR pair there) and the kernel does not
   emulate it, so both clients died with a program exception in the entropy
   paths -- GrimReaper put one crash at note_input_event (first keystroke) and
   one inside drbg_generate during auth.sendCode (tester crashlogs, A1222).
   Read the E-Clock through timer.device instead: a legal user-mode API with
   the same jitter quality on EVERY OS4 machine (G3/G4/X5000/A1222). The
   device is opened lazily once and stays open for the process lifetime; if
   the open ever fails, gettimeofday microseconds keep the mix alive. */
static struct MsgPort *tg_os4_tb_port = 0;
static struct TimeRequest *tg_os4_tb_req = 0;
static struct Library *tg_os4_tb_base = 0;
static struct TimerIFace *tg_os4_itimer = 0;
static int tg_os4_tb_device_open = 0;
static int tg_os4_tb_state = 0; /* 0 untried, 1 EClock ready, -1 fallback */

static void tg_os4_timebase_close(void)
{
    if (tg_os4_itimer != 0) {
        DropInterface((struct Interface *)tg_os4_itimer);
        tg_os4_itimer = 0;
    }
    if (tg_os4_tb_device_open && tg_os4_tb_req != 0) {
        CloseDevice((struct IORequest *)tg_os4_tb_req);
        tg_os4_tb_device_open = 0;
    }
    tg_os4_tb_base = 0;
    if (tg_os4_tb_req != 0) {
        FreeSysObject(ASOT_IOREQUEST, tg_os4_tb_req);
        tg_os4_tb_req = 0;
    }
    if (tg_os4_tb_port != 0) {
        FreeSysObject(ASOT_PORT, tg_os4_tb_port);
        tg_os4_tb_port = 0;
    }
    tg_os4_tb_state = 0;
}

static void tg_os4_socket_shutdown(void);

void tg_platform_shutdown(void)
{
    tg_os4_timebase_close();
    tg_os4_socket_shutdown();
}

static unsigned long tg_os4_timebase(void)
{
    if (tg_os4_tb_state == 0) {
        tg_os4_tb_state = -1;
        /* AllocSysObject family: the OS4-native replacement for
           CreateMsgPort/CreateIORequest (community PR #10). ASOIOR_ReplyPort
           keeps the request bound to the port exactly as CreateIORequest
           did. */
        tg_os4_tb_port = (struct MsgPort *)AllocSysObject(ASOT_PORT, NULL);
        if (tg_os4_tb_port != 0) {
            tg_os4_tb_req = (struct TimeRequest *)AllocSysObjectTags(
                ASOT_IOREQUEST, ASOIOR_Size, sizeof(struct TimeRequest),
                ASOIOR_ReplyPort, tg_os4_tb_port, TAG_DONE);
        }
        if (tg_os4_tb_req != 0 &&
            OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                       (struct IORequest *)tg_os4_tb_req, 0) == 0) {
            tg_os4_tb_device_open = 1;
            tg_os4_tb_base =
                (struct Library *)tg_os4_tb_req->Request.io_Device;
            tg_os4_itimer = (struct TimerIFace *)GetInterface(
                tg_os4_tb_base, "main", 1L, 0);
            if (tg_os4_itimer != 0) {
                tg_os4_tb_state = 1;
            }
        }
    }
    if (tg_os4_tb_state == 1) {
        struct EClockVal ecv;

        tg_os4_itimer->ReadEClock(&ecv);
        return ecv.ev_lo ^ (ecv.ev_hi << 16);
    }
    {
        struct timeval tv;

        gettimeofday(&tv, 0);
        return (unsigned long)tv.tv_usec ^
               ((unsigned long)tv.tv_sec << 20);
    }
}

static unsigned long tg_os4_entropy_gather(unsigned char *buf, unsigned long cap)
{
    unsigned long n = 0;
    unsigned long i;
    struct timeval tv;
    time_t now;
    void *p;
    struct Task *task;

    now = time(0);
    if (n + sizeof(now) <= cap) {
        memcpy(buf + n, &now, sizeof(now));
        n += sizeof(now);
    }

    /* High-resolution time-base sampled in a variable-duration jitter loop. */
    for (i = 0; i < 96UL && n + sizeof(unsigned long) <= cap; ++i) {
        unsigned long tb = tg_os4_timebase();
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
    p = (void *)IExec;
    if (n + sizeof(p) <= cap) { memcpy(buf + n, &p, sizeof(p)); n += sizeof(p); }
    task = FindTask(0);
    if (n + sizeof(task) <= cap) { memcpy(buf + n, &task, sizeof(task)); n += sizeof(task); }

    return n;
}

static void tg_os4_drbg_generate(unsigned char *out, unsigned long n);

static FILE *tg_os4_open_seed_file(const char *mode)
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
static void tg_os4_drbg_seed(void)
{
    unsigned char pool[1024];
    unsigned long len = tg_os4_entropy_gather(pool, sizeof(pool));
    {
        FILE *seed_file = tg_os4_open_seed_file("rb");
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
    tg_mtproto_sha256(pool, len, tg_os4_drbg_state);
    tg_os4_drbg_ready = 1;
    {
        unsigned char fresh[64];
        FILE *seed_file;
        tg_os4_drbg_generate(fresh, sizeof(fresh));
        seed_file = tg_os4_open_seed_file("wb");
        if (seed_file != 0) {
            fwrite(fresh, 1U, sizeof(fresh), seed_file);
            fclose(seed_file);
        }
        memset(fresh, 0, sizeof(fresh));
    }
}

static void tg_os4_drbg_generate(unsigned char *out, unsigned long n)
{
    static unsigned long calls = 0;
    unsigned char key[TG_MTPROTO_SHA256_LENGTH];
    unsigned char block[TG_MTPROTO_SHA256_LENGTH];
    unsigned long off;
    unsigned long ctr;

    if (!tg_os4_drbg_ready) {
        tg_os4_drbg_seed();
    }

    /* Derive a per-call working key from the state plus fresh entropy. */
    {
        unsigned char work[TG_MTPROTO_SHA256_LENGTH + 32 +
                           sizeof(tg_os4_key_ring) + sizeof(unsigned long)];
        unsigned long wn = TG_MTPROTO_SHA256_LENGTH;
        struct timeval tv;
        unsigned long tb = tg_os4_timebase();
        memcpy(work, tg_os4_drbg_state, TG_MTPROTO_SHA256_LENGTH);
        gettimeofday(&tv, 0);
        ++calls;
        memcpy(work + wn, &tv, sizeof(tv)); wn += sizeof(tv);
        memcpy(work + wn, &tb, sizeof(tb)); wn += sizeof(tb);
        memcpy(work + wn, &calls, sizeof(calls)); wn += sizeof(calls);
        /* Keystroke-timing ring: human input collected since the last
           generate (cheap on the input path, absorbed here). */
        memcpy(work + wn, tg_os4_key_ring, sizeof(tg_os4_key_ring));
        wn += sizeof(tg_os4_key_ring);
        memcpy(work + wn, &tg_os4_key_ring_pos,
               sizeof(tg_os4_key_ring_pos));
        wn += sizeof(tg_os4_key_ring_pos);
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

    /* Ratchet the persistent state forward. */
    {
        unsigned char rb[TG_MTPROTO_SHA256_LENGTH + 1];
        memcpy(rb, key, TG_MTPROTO_SHA256_LENGTH);
        rb[TG_MTPROTO_SHA256_LENGTH] = 0x01;
        tg_mtproto_sha256(rb, sizeof(rb), tg_os4_drbg_state);
    }
}
#endif /* __amigaos4__ */

int tg_platform_random_bytes(unsigned char *bytes, unsigned long byte_count)
{
#if defined(__amigaos4__)
    if (bytes == 0) {
        return 0;
    }
    if (byte_count == 0) {
        return 1;
    }
    tg_os4_drbg_generate(bytes, byte_count);
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

#if defined(__amigaos4__)

static void tg_platform_set_error(char *error_buffer, unsigned long error_buffer_size,
                                  const char *message)
{
    if (error_buffer != 0 && error_buffer_size > 0) {
        strncpy(error_buffer, message, error_buffer_size - 1);
        error_buffer[error_buffer_size - 1] = '\0';
    }
}

static int tg_amigaos4_socket_open(char *error_buffer, unsigned long error_buffer_size)
{
    if (SocketBase != 0 && ISocket != 0) {
        return 0;
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (SocketBase == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not open bsdsocket.library v4");
        return 1;
    }

    ISocket = (struct SocketIFace *)GetInterface((struct Library *)SocketBase,
                                                 "main", 1L, 0);
    if (ISocket == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not get bsdsocket.library interface");
        CloseLibrary(SocketBase);
        SocketBase = 0;
        return 1;
    }

    return 0;
}

static void tg_amigaos4_socket_close_library(void)
{
    if (ISocket != 0) {
        DropInterface((struct Interface *)ISocket);
        ISocket = 0;
    }
    if (SocketBase != 0) {
        CloseLibrary(SocketBase);
        SocketBase = 0;
    }
}

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

    if (tg_amigaos4_socket_open(error_buffer, error_buffer_size) != 0) {
        return TG_NET_CONNECT_FAILED;
    }

    host_entry = gethostbyname((char *)host);
    if (host_entry == 0 || host_entry->h_addr_list == 0 ||
        host_entry->h_addr_list[0] == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size, "host lookup failed");
        return TG_NET_RESOLVE_FAILED;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port_number);
    address.sin_len = (unsigned char)sizeof(address);
    memcpy(&address.sin_addr, host_entry->h_addr_list[0], (size_t)host_entry->h_length);

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

    CloseSocket(sock);
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

    rc = send((int)connection->platform_handle, (void *)data, (long)byte_count, 0);
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

    /* Bound the blocking recv() with a WaitSelect() timeout, mirroring the
       MorphOS/AROS backends. Without this the encrypted-query receive loop in
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
    rc = WaitSelect((int)connection->platform_handle + 1, &read_fds, 0, 0,
                    &timeout, 0);
    if (rc <= 0 || !FD_ISSET((int)connection->platform_handle, &read_fds)) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              rc == 0 ? "socket receive timed out"
                                      : "socket receive failed");
        return rc == 0 ? TG_NET_TIMEOUT : TG_NET_RECV_FAILED;
    }

    rc = recv((int)connection->platform_handle, buffer, (long)buffer_size, 0);
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
    rc = WaitSelect((int)connection->platform_handle + 1, &read_fds, 0, 0,
                    &timeout, 0);
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
        CloseSocket((int)connection->platform_handle);
        connection->is_open = 0;
    }
    /* The shared ISocket/SocketBase stays open for the other live connections;
       it is released once, in tg_platform_shutdown() (or by the AmiSSL
       teardown when that owns it). Per-close CloseLibrary zeroed the base
       under live connections (reproduced as a relaunch bus-fault on AROS). */
}

/* Process-exit mirror of the lazy open in tcp_connect (non-AmiSSL builds;
   the AmiSSL teardown closes the base itself when enabled). */
static void tg_os4_socket_shutdown(void)
{
#if !TG_AMIGAOS4_ENABLE_AMISSL
    tg_amigaos4_socket_close_library();
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

#if defined(__amigaos4__) && TG_AMIGAOS4_ENABLE_AMISSL

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

static tg_tls_status tg_amigaos4_configure_certificate_validation(
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

static void tg_amigaos4_amissl_cleanup(void)
{
    if (tg_amigaos4_amissl_initialized) {
        CleanupAmiSSLA(0);
        tg_amigaos4_amissl_initialized = 0;
    }

    if (IAmiSSL != 0) {
        CloseAmiSSL();
        IAmiSSL = 0;
    }

    if (IAmiSSLMaster != 0) {
        DropInterface((struct Interface *)IAmiSSLMaster);
        IAmiSSLMaster = 0;
    }

    if (AmiSSLMasterBase != 0) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = 0;
    }

    tg_amigaos4_socket_close_library();
}

static int tg_amigaos4_amissl_init(char *error_buffer, unsigned long error_buffer_size)
{
    long amissl_error;
    char detail[80];

    if (tg_amigaos4_amissl_initialized) {
        return 0;
    }

    if (tg_amigaos4_socket_open(error_buffer, error_buffer_size) != 0) {
        tg_amigaos4_amissl_cleanup();
        return 1;
    }

    AmiSSLMasterBase = OpenLibrary((CONST_STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (AmiSSLMasterBase == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not open amisslmaster.library");
        tg_amigaos4_amissl_cleanup();
        return 1;
    }

    IAmiSSLMaster = (struct AmiSSLMasterIFace *)
        GetInterface((struct Library *)AmiSSLMasterBase, "main", 1L, 0);
    if (IAmiSSLMaster == 0) {
        tg_platform_set_error(error_buffer, error_buffer_size,
                              "could not get AmiSSLMaster interface");
        tg_amigaos4_amissl_cleanup();
        return 1;
    }

    amissl_error = OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                                  AmiSSL_UsesOpenSSLStructs, FALSE,
                                  AmiSSL_GetIAmiSSL, &IAmiSSL,
                                  AmiSSL_ISocket, ISocket,
                                  AmiSSL_ErrNoPtr, &errno,
                                  TAG_DONE);
    if (amissl_error != 0) {
        sprintf(detail, "could not initialize AmiSSL (%ld)", amissl_error);
        tg_platform_set_error(error_buffer, error_buffer_size, detail);
        tg_amigaos4_amissl_cleanup();
        return 1;
    }

    tg_amigaos4_amissl_initialized = 1;
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

    if (tg_amigaos4_amissl_init(error_buffer, error_buffer_size) != 0) {
        return TG_TLS_HANDSHAKE_FAILED;
    }

    local_net_status = tg_net_connect(&connection->tcp, host, port,
                                      error_buffer, error_buffer_size);
    if (local_net_status != TG_NET_OK) {
        if (net_status != 0) {
            *net_status = local_net_status;
        }
        tg_amigaos4_amissl_cleanup();
        return TG_TLS_NET_ERROR;
    }

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "TLS context creation failed");
        tg_net_close(&connection->tcp);
        tg_amigaos4_amissl_cleanup();
        return TG_TLS_HANDSHAKE_FAILED;
    }

#ifdef SSL_MODE_AUTO_RETRY
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
#endif
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    ssl = SSL_new(ctx);
    if (ssl == 0) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "TLS session creation failed");
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos4_amissl_cleanup();
        return TG_TLS_HANDSHAKE_FAILED;
    }

    SSL_set_fd(ssl, (int)connection->tcp.platform_handle);
    SSL_set_tlsext_host_name(ssl, host);
    verify_status = tg_amigaos4_configure_certificate_validation(
        ctx, ssl, host, error_buffer, error_buffer_size);
    if (verify_status != TG_TLS_OK) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos4_amissl_cleanup();
        return verify_status;
    }
    ERR_clear_error();

    if (SSL_connect(ssl) != 1) {
        tg_platform_set_ssl_error(error_buffer, error_buffer_size,
                                  "TLS handshake failed");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        tg_net_close(&connection->tcp);
        tg_amigaos4_amissl_cleanup();
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
        tg_amigaos4_amissl_cleanup();
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
        tg_platform_set_ssl_error(error_buffer, error_buffer_size, "TLS send failed");
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
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return TG_TLS_CLOSED;
    }

    tg_platform_set_ssl_error(error_buffer, error_buffer_size, "TLS receive failed");
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
    connection->platform_session = 0;
    connection->platform_context = 0;
    connection->is_open = 0;
    tg_amigaos4_amissl_cleanup();
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
#if defined(__amigaos4__)
    /* Peek without clearing: the break stays pending for outer loops. */
    return (SetSignal(0L, 0L) & SIGBREAKF_CTRL_C) != 0L;
#else
    return 0;
#endif
}

#if defined(__amigaos4__)
#include <proto/intuition.h>

struct Library *IntuitionBase = 0;
struct IntuitionIFace *IIntuition = 0;
#endif

/* Clear the "e" protection bit on a completed download (issue #15). The RWED
   bits are active low, so a file is runnable when FIBB_EXECUTE is CLEAR;
   read-modify-write keeps the archive bit and the others as the filesystem
   left them. Any failure is silent: the download itself already succeeded and
   the user can still `protect +e` by hand. */
void tg_platform_set_executable(const char *path)
{
#if defined(__amigaos4__)
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
#if defined(__amigaos4__)
    /* The screen flash is the Amiga-native notification; a BEL byte lets
       console handlers improvise (AmiKit's console clears the window). */
    /* The GUI owns this global base/interface for its whole window lifetime.
       Borrow them when already open; otherwise acquire and release only the
       references created here. Overwriting the held pointers made the first
       notification close Intuition out from under the live OS4 window. */
    int opened_base = 0;
    int opened_interface = 0;

    if (IntuitionBase == 0) {
        IntuitionBase = OpenLibrary("intuition.library", 0L);
        opened_base = 1;
    }
    if (IntuitionBase != 0) {
        if (IIntuition == 0) {
            IIntuition = (struct IntuitionIFace *)GetInterface(
                IntuitionBase, "main", 1L, 0);
            opened_interface = IIntuition != 0;
        }
        if (IIntuition != 0) {
            DisplayBeep(0);
        }
        if (opened_interface) {
            DropInterface((struct Interface *)IIntuition);
            IIntuition = 0;
        }
        if (opened_base) {
            CloseLibrary(IntuitionBase);
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
        struct IconIFace *iicon =
            (struct IconIFace *)GetInterface(IconBase, "main", 1L, 0);
        if (iicon == 0) {
            CloseLibrary(IconBase);
            return;
        }
        {
        struct IconIFace *IIcon = iicon; (void)IIcon;
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
        DropInterface((struct Interface *)iicon);
    }
    CloseLibrary(IconBase);
}

static BPTR tg_wb_tui_con = 0;
static BPTR tg_wb_tui_old_in = 0;
static BPTR tg_wb_tui_old_out = 0;
static struct MsgPort *tg_wb_tui_old_ct = 0;

static void tg_os4_drop_arm(void);
static void tg_os4_drop_disarm(void);

#define TG_OS4_TUI_TITLE "Telegram Amiga TUI"

int tg_platform_workbench_tui_console(void)
{
    BPTR con;

    con = Open((CONST_STRPTR)"CON:20/20/640/440/" TG_OS4_TUI_TITLE
               "/CLOSE", MODE_OLDFILE);
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
#if defined(__amigaos4__)
    /* OS4 renamed the call and the FileHandle member. */
    tg_wb_tui_old_ct =
        SetConsolePort(((struct FileHandle *)BADDR(con))->fh_MsgPort);
#else
    SetConsoleTask(((struct FileHandle *)BADDR(con))->fh_Type);
#endif
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
    tg_os4_drop_arm(); /* best-effort file drag-and-drop onto this window */
    return 1;
}

void tg_platform_workbench_tui_console_close(void)
{
    if (tg_wb_tui_con == 0) {
        return; /* console never opened (CLI launch or open failure) */
    }
    tg_os4_drop_disarm();
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
    SetConsolePort(tg_wb_tui_old_ct);
    Close(tg_wb_tui_con);
    tg_wb_tui_con = 0;
}

/* ---- Workbench TUI drag-and-drop (AppWindow, OS4-only) ------------------- */

/* We call these four through OUR OWN stored interface pointers, so (now that
   every header is in) drop the __USE_INLINE__ macros that would rewrite them
   as GlobalIface->method. IExec/IDoS calls elsewhere keep their macros -- those
   really are the auto-provided globals. */
#undef LockPubScreen
#undef UnlockPubScreen
#undef AddAppWindowA
#undef RemoveAppWindow
#undef AddAppIconA
#undef RemoveAppIcon
#undef GetDiskObject
#undef GetDefDiskObject
#undef FreeDiskObject

static const char *tg_os4_drop_diag = "not armed";
static unsigned long tg_os4_drop_polls = 0;  /* poll calls (loop alive?) */
static unsigned long tg_os4_drop_msgs = 0;   /* AppMessages ever received */

static struct Library *tg_os4_wb_base = 0;
static struct WorkbenchIFace *tg_os4_iwb = 0;
static struct Library *tg_os4_int_base = 0;
static struct IntuitionIFace *tg_os4_iint = 0;
static struct MsgPort *tg_os4_app_port = 0;
static struct AppWindow *tg_os4_app_win = 0;
static struct AppIcon *tg_os4_app_icon = 0;
static struct Library *tg_os4_icon_base = 0;
static struct IconIFace *tg_os4_iicon = 0;
static struct DiskObject *tg_os4_drop_dobj = 0;

/* Confirm `cand` is a live window on the default public screen before handing
   it to workbench.library. The DoPkt(ACTION_DISK_INFO) trick that yields the
   CON: window pointer is not guaranteed on every handler (OS4's con-handler
   is a rewrite), and a bogus pointer into AddAppWindow would crash; if it is
   not in the screen's window list we simply do not arm drag-and-drop. */
static int tg_os4_window_is_live(struct Window *cand)
{
    struct Screen *scr;
    struct Window *w;
    int found = 0;

    if (cand == 0 || tg_os4_iint == 0) {
        return 0;
    }
    scr = tg_os4_iint->LockPubScreen(0);
    if (scr == 0) {
        return 0;
    }
    /* LockPubScreen keeps the screen alive but not its window list: Forbid()
       around the walk so no other task opens/closes a window mid-traversal
       (a closing window would dangle NextWindow). */
    Forbid();
    for (w = scr->FirstWindow; w != 0; w = w->NextWindow) {
        if (w == cand) {
            found = 1;
            break;
        }
    }
    Permit();
    tg_os4_iint->UnlockPubScreen(0, scr);
    return found;
}

static void tg_os4_drop_disarm(void)
{
    if (tg_os4_app_icon != 0 && tg_os4_iwb != 0) {
        tg_os4_iwb->RemoveAppIcon(tg_os4_app_icon);
        tg_os4_app_icon = 0;
    }
    if (tg_os4_app_win != 0 && tg_os4_iwb != 0) {
        tg_os4_iwb->RemoveAppWindow(tg_os4_app_win);
        tg_os4_app_win = 0;
    }
    if (tg_os4_app_port != 0) {
        struct Message *m;

        while ((m = GetMsg(tg_os4_app_port)) != 0) {
            ReplyMsg(m); /* drain and reply un-handled drops */
        }
        FreeSysObject(ASOT_PORT, tg_os4_app_port);
        tg_os4_app_port = 0;
    }
    if (tg_os4_drop_dobj != 0 && tg_os4_iicon != 0) {
        tg_os4_iicon->FreeDiskObject(tg_os4_drop_dobj);
        tg_os4_drop_dobj = 0;
    }
    if (tg_os4_iicon != 0) {
        DropInterface((struct Interface *)tg_os4_iicon);
        tg_os4_iicon = 0;
    }
    if (tg_os4_icon_base != 0) {
        CloseLibrary(tg_os4_icon_base);
        tg_os4_icon_base = 0;
    }
    if (tg_os4_iwb != 0) {
        DropInterface((struct Interface *)tg_os4_iwb);
        tg_os4_iwb = 0;
    }
    if (tg_os4_wb_base != 0) {
        CloseLibrary(tg_os4_wb_base);
        tg_os4_wb_base = 0;
    }
    if (tg_os4_iint != 0) {
        DropInterface((struct Interface *)tg_os4_iint);
        tg_os4_iint = 0;
    }
    if (tg_os4_int_base != 0) {
        CloseLibrary(tg_os4_int_base);
        tg_os4_int_base = 0;
    }
}

/* Fallback window lookup: the CON: title is OURS (we put it in the spec), so
   a title match on the public screen finds the console window even when the
   handler does not answer the DISK_INFO trick. The window lives until the
   console is closed, so the pointer stays valid for the AppWindow's life. */
static struct Window *tg_os4_find_window_by_title(const char *title)
{
    struct Screen *scr;
    struct Window *w;
    struct Window *found = 0;

    if (tg_os4_iint == 0) {
        return 0;
    }
    scr = tg_os4_iint->LockPubScreen(0);
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
    tg_os4_iint->UnlockPubScreen(0, scr);
    return found;
}

static void tg_os4_drop_arm(void)
{
    struct InfoData id;
    struct FileHandle *fh;
    struct Window *win;

    if (tg_wb_tui_con == 0) {
        tg_os4_drop_diag = "no Workbench console";
        return; /* only the console we opened from Workbench */
    }
    if (tg_os4_app_win != 0 || tg_os4_app_port != 0 ||
        tg_os4_wb_base != 0 || tg_os4_int_base != 0) {
        return; /* already armed: never leak a second set of resources */
    }
    fh = (struct FileHandle *)BADDR(tg_wb_tui_con);
    if (fh == 0) {
        return;
    }
    tg_os4_int_base = OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (tg_os4_int_base == 0) {
        tg_os4_drop_diag = "intuition.library open failed";
        return;
    }
    tg_os4_iint = (struct IntuitionIFace *)GetInterface(tg_os4_int_base,
                                                        "main", 1L, 0);
    if (tg_os4_iint == 0) {
        tg_os4_drop_diag = "intuition interface failed";
        tg_os4_drop_disarm();
        return;
    }
    memset(&id, 0, sizeof(id));
    /* Two ways to find OUR console window, in order of directness:
       1. The CON: handler answers ACTION_DISK_INFO by stuffing a RAW window
          pointer into the (BPTR-typed) id_VolumeNode field -- the documented
          trick, read back as a PLAIN cast (BADDR would shift it x4). The
          pointer is trusted only if it shows up in the screen window list.
       2. If the handler does not play the trick (OS4's is a rewrite), find
          the window by its TITLE: it is ours, set in the CON: spec above. */
    win = 0;
    if (DoPkt(fh->fh_MsgPort, ACTION_DISK_INFO, (LONG)MKBADDR(&id),
              0, 0, 0, 0)) {
        win = (struct Window *)id.id_VolumeNode;
    }
    if (tg_os4_window_is_live(win)) {
        tg_os4_drop_diag = "ready (handler window)";
    } else {
        win = tg_os4_find_window_by_title(TG_OS4_TUI_TITLE);
        /* NB a STALE console window with the same title (an old WAIT window
           from a previous run) would be matched first: the diag says which
           route armed so a field report can catch exactly that. */
        tg_os4_drop_diag = "ready (title match)";
    }
    if (win == 0) {
        tg_os4_drop_diag = "console window not found";
        tg_os4_drop_disarm(); /* no drop, but no crash */
        return;
    }
    tg_os4_wb_base = OpenLibrary((CONST_STRPTR)"workbench.library", 44);
    if (tg_os4_wb_base == 0) {
        tg_os4_drop_diag = "workbench.library open failed";
        tg_os4_drop_disarm();
        return;
    }
    tg_os4_iwb = (struct WorkbenchIFace *)GetInterface(tg_os4_wb_base, "main",
                                                       1L, 0);
    if (tg_os4_iwb == 0) {
        tg_os4_drop_diag = "workbench interface failed";
        tg_os4_drop_disarm();
        return;
    }
    tg_os4_app_port = (struct MsgPort *)AllocSysObject(ASOT_PORT, NULL);
    if (tg_os4_app_port == 0) {
        tg_os4_drop_diag = "message port failed";
        tg_os4_drop_disarm();
        return;
    }
    tg_os4_app_win = tg_os4_iwb->AddAppWindowA(0UL, 0UL, win, tg_os4_app_port,
                                               0);
    if (tg_os4_app_win == 0) {
        tg_os4_drop_diag = "AddAppWindow failed";
        tg_os4_drop_disarm();
        return;
    }
    /* Field result on OS4: the AppWindow registers fine but Workbench never
       delivers console-window drops to it (the con-handler owns those). The
       reliable lane is an APPICON on the same message port: drop the file on
       the TelegramAmiga icon that appears on the Workbench while the console
       client runs. Same port, same poll, same injection. */
    tg_os4_icon_base = OpenLibrary((CONST_STRPTR)"icon.library", 36L);
    if (tg_os4_icon_base != 0) {
        tg_os4_iicon = (struct IconIFace *)GetInterface(tg_os4_icon_base,
                                                        "main", 1L, 0);
    }
    if (tg_os4_iicon != 0) {
        tg_os4_drop_dobj =
            tg_os4_iicon->GetDiskObject((STRPTR)"PROGDIR:TelegramAmiga");
        if (tg_os4_drop_dobj == 0) {
            tg_os4_drop_dobj = tg_os4_iicon->GetDefDiskObject(WBTOOL);
        }
        if (tg_os4_drop_dobj != 0) {
            tg_os4_drop_dobj->do_CurrentX = NO_ICON_POSITION;
            tg_os4_drop_dobj->do_CurrentY = NO_ICON_POSITION;
            tg_os4_drop_dobj->do_Type = 0; /* plain AppIcon, not a WB object */
            tg_os4_app_icon = tg_os4_iwb->AddAppIconA(
                0UL, 0UL, (CONST_STRPTR)"TG drop", tg_os4_app_port, 0,
                tg_os4_drop_dobj, 0);
        }
    }
    /* the diag already says WHICH route found the window; the icon lane is
       best-effort on top (drops on either land on the same port) */
}

const char *tg_platform_console_drop_diag(void)
{
    /* Live counters make the field report decisive: polls==0 means the read
       loop never asks, msgs==0 means Workbench never delivered (e.g. the
       con-handler's own drop handling swallowed it). */
    static char diag_buf[96];

    sprintf(diag_buf, "%s, polls %lu, drops %lu", tg_os4_drop_diag,
            tg_os4_drop_polls, tg_os4_drop_msgs);
    return diag_buf;
}

int tg_platform_console_drop_poll(char *out, unsigned long out_size)
{
    struct AppMessage *am;
    int got = 0;

    if (out == 0 || out_size == 0UL) {
        return 0;
    }
    out[0] = '\0';
    if (tg_os4_app_port == 0) {
        return 0;
    }
    ++tg_os4_drop_polls;
    while ((am = (struct AppMessage *)GetMsg(tg_os4_app_port)) != 0) {
        ++tg_os4_drop_msgs;
        /* Take the FIRST dropped file only (one path per /sendfile); still
           reply to every message so the sender is never left blocked. */
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
        sprintf(cmd, "URLOpen \"%s\"", url);
        rc = (int)SystemTags((STRPTR)cmd, SYS_Input, (ULONG)nil_in,
                             SYS_Output, (ULONG)nil_out, TAG_DONE);
    }
    if (rc != 0) {
        sprintf(cmd, "OpenURL \"%s\"", url);
        rc = (int)SystemTags((STRPTR)cmd, SYS_Input, (ULONG)nil_in,
                             SYS_Output, (ULONG)nil_out, TAG_DONE);
    }
    Close(nil_in);
    Close(nil_out);
    return rc;
}


/* --- GUI drag-and-drop (0.0.8 punto 1e): our own Intuition window --------
   Unlike the console (whose CON: window belongs to the system, hence the
   AppIcon detour above), the GUI window is OURS: a plain AppWindow on it
   receives drops fine. Same port and poll as the console lane; single
   owner process-wide. */
int tg_platform_gui_drop_arm(void *window)
{
    if (window == 0) {
        return -1;
    }
    if (tg_os4_app_port != 0 || tg_os4_app_win != 0) {
        return 0; /* already armed */
    }
    if (tg_os4_wb_base == 0) {
        tg_os4_wb_base = OpenLibrary((CONST_STRPTR)"workbench.library", 44);
        if (tg_os4_wb_base == 0) {
            tg_os4_drop_diag = "workbench.library open failed";
            return -1;
        }
    }
    if (tg_os4_iwb == 0) {
        tg_os4_iwb = (struct WorkbenchIFace *)GetInterface(tg_os4_wb_base,
                                                           "main", 1L, 0);
        if (tg_os4_iwb == 0) {
            tg_os4_drop_diag = "workbench interface failed";
            tg_os4_drop_disarm();
            return -1;
        }
    }
    tg_os4_app_port = (struct MsgPort *)AllocSysObject(ASOT_PORT, NULL);
    if (tg_os4_app_port == 0) {
        tg_os4_drop_diag = "message port failed";
        tg_os4_drop_disarm();
        return -1;
    }
    tg_os4_app_win = tg_os4_iwb->AddAppWindowA(0UL, 0UL,
                                               (struct Window *)window,
                                               tg_os4_app_port, 0);
    if (tg_os4_app_win == 0) {
        tg_os4_drop_diag = "AddAppWindow failed";
        tg_os4_drop_disarm();
        return -1;
    }
    tg_os4_drop_diag = "ready (GUI window)";
    return 0;
}

void tg_platform_gui_drop_disarm(void)
{
    tg_os4_drop_disarm();
}

unsigned long tg_platform_gui_drop_sigmask(void)
{
    return (tg_os4_app_port != 0)
               ? (1UL << tg_os4_app_port->mp_SigBit) : 0UL;
}
