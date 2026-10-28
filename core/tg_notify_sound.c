/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include "tg_notify.h"
#include "tg_platform.h"
#include <string.h>

#if (defined(__amigaos3__) || defined(__amigaos4__) || \
     defined(__MORPHOS__) || defined(__AROS__)) && !defined(TG_LOWMEM)

#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif
#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/soundclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#if defined(__amigaos3__)
#include <devices/audio.h>
#include <clib/alib_protos.h>
#endif
#if defined(__amigaos4__)
#include <exec/exectags.h>
#include <dos/obsolete.h>
#endif
#if defined(__MORPHOS__)
#include <exec/system.h>
#endif
#if defined(__AROS__)
#include <datatypes/soundclassext.h>
#define TG_NOTIFY_TAG(p) ((IPTR)(p))
#else
#define TG_NOTIFY_TAG(p) ((ULONG)(p))
#endif

/* Opened only while a sound file plays (tg_notify_sound_start/stop). */
struct Library *DataTypesBase = 0;
#if defined(__amigaos4__)
struct DataTypesIFace *IDataTypes = 0;
#endif

#if defined(__has_include)
#if __has_include(<devices/ahi.h>)
#define TG_NOTIFY_HAVE_AHI_H 1
#endif
#endif
#if defined(TG_NOTIFY_HAVE_AHI_H)
#include <devices/ahi.h>
#else
/* No AHI header in this SDK (the AmigaOS 3 NDK, the Raspberry Pi AROS):
   the request as devices/ahi.h defines it, for the systems where AHI is
   installed all the same. */
#define AHINAME "ahi.device"
#define AHI_DEFAULT_UNIT 0U
#define AHIST_M8S 0UL
#define AHIST_M16S 1UL
#define AHIST_S8S 2UL
#define AHIST_S16S 3UL
typedef LONG Fixed;
struct AHIRequest {
    struct IOStdReq ahir_Std;
    UWORD ahir_Version;
    UWORD ahir_Pad1;
#if defined(__AROS__)
    IPTR ahir_Private[2];
#else
    ULONG ahir_Private[2];
#endif
    ULONG ahir_Type;
    ULONG ahir_Frequency;
    Fixed ahir_Volume;
    Fixed ahir_Position;
    struct AHIRequest *ahir_Link;
};
#endif
static struct MsgPort *tg_notify_chime_port(void)
{
#if defined(__amigaos4__)
    return (struct MsgPort *)AllocSysObjectTags(ASOT_PORT, TAG_END);
#else
    return CreateMsgPort();
#endif
}

static void tg_notify_chime_port_free(struct MsgPort *port)
{
#if defined(__amigaos4__)
    FreeSysObject(ASOT_PORT, port);
#else
    DeleteMsgPort(port);
#endif
}

static struct IORequest *tg_notify_chime_request(struct MsgPort *port,
                                              ULONG size)
{
#if defined(__amigaos4__)
    return (struct IORequest *)AllocSysObjectTags(
        ASOT_IOREQUEST, ASOIOR_Size, size, ASOIOR_ReplyPort, port, TAG_END);
#else
    return CreateIORequest(port, size);
#endif
}

static void tg_notify_chime_request_free(struct IORequest *io)
{
#if defined(__amigaos4__)
    FreeSysObject(ASOT_IOREQUEST, io);
#else
    DeleteIORequest(io);
#endif
}

static int tg_notify_chime_ahi(signed char *wave)
{
    struct MsgPort *port;
    struct AHIRequest *ahi;
    int played = 0;

    port = tg_notify_chime_port();
    if (port == 0) {
        return 0;
    }
    ahi = (struct AHIRequest *)tg_notify_chime_request(
        port, (ULONG)sizeof(struct AHIRequest));
    if (ahi != 0) {
        ahi->ahir_Version = 4;
        if (OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT,
                       (struct IORequest *)ahi, 0) == 0) {
            ahi->ahir_Std.io_Command = CMD_WRITE;
            ahi->ahir_Std.io_Data = (APTR)wave;
            ahi->ahir_Std.io_Length = (ULONG)TG_NOTIFY_CHIME_LENGTH;
            ahi->ahir_Std.io_Offset = 0;
            ahi->ahir_Type = AHIST_M8S;
            ahi->ahir_Frequency = (ULONG)TG_NOTIFY_CHIME_RATE;
            ahi->ahir_Volume = 0xC000;   /* three quarters */
            ahi->ahir_Position = 0x8000; /* centre */
            ahi->ahir_Link = 0;
            played = DoIO((struct IORequest *)ahi) == 0;
            CloseDevice((struct IORequest *)ahi);
        }
        tg_notify_chime_request_free((struct IORequest *)ahi);
    }
    tg_notify_chime_port_free(port);
    return played;
}

#if defined(__amigaos3__)
/* Paula: one free channel of any side, the wave in chip RAM. */
static int tg_notify_chime_paula(signed char *wave)
{
    static UBYTE channels[] = { 1, 8, 2, 4 };
    struct MsgPort *port;
    struct IOAudio *audio;
    int played = 0;

    port = tg_notify_chime_port();
    if (port == 0) {
        return 0;
    }
    audio = (struct IOAudio *)tg_notify_chime_request(
        port, (ULONG)sizeof(struct IOAudio));
    if (audio != 0) {
        audio->ioa_Request.io_Message.mn_Node.ln_Pri = 0;
        audio->ioa_Data = channels; /* the open allocates a channel */
        audio->ioa_Length = (ULONG)sizeof(channels);
        if (OpenDevice((CONST_STRPTR)AUDIONAME, 0,
                       (struct IORequest *)audio, 0) == 0) {
            audio->ioa_Request.io_Command = CMD_WRITE;
            audio->ioa_Request.io_Flags = ADIOF_PERVOL;
            audio->ioa_Data = (UBYTE *)wave;
            audio->ioa_Length = (ULONG)TG_NOTIFY_CHIME_LENGTH;
            audio->ioa_Period = 161; /* 3546895 Hz / 161 = 22 kHz (PAL) */
            audio->ioa_Volume = 48;
            audio->ioa_Cycles = 1;
            /* BeginIO, not DoIO: exec's DoIO sets io_Flags to IOF_QUICK,
               and without ADIOF_PERVOL the channel keeps a period and a
               volume of nought, silent though the request succeeds. */
            BeginIO((struct IORequest *)audio);
            played = WaitIO((struct IORequest *)audio) == 0;
            CloseDevice((struct IORequest *)audio); /* frees the channel */
        }
        tg_notify_chime_request_free((struct IORequest *)audio);
    }
    tg_notify_chime_port_free(port);
    return played;
}
#endif

/* 1 when the chime played. */
static int tg_notify_chime_play(void)
{
    signed char *wave;
    int played = 0;

    wave = (signed char *)AllocVec((ULONG)TG_NOTIFY_CHIME_LENGTH, MEMF_PUBLIC);
    if (wave != 0) {
        tg_notify_chime_render(wave);
        played = tg_notify_chime_ahi(wave);
        FreeVec(wave);
    }
    if (played) {
        return 1;
    }
#if defined(__amigaos3__)
    wave = (signed char *)AllocMem((ULONG)TG_NOTIFY_CHIME_LENGTH, MEMF_CHIP);
    if (wave != 0) {
        tg_notify_chime_render(wave);
        played = tg_notify_chime_paula(wave);
        FreeMem(wave, (ULONG)TG_NOTIFY_CHIME_LENGTH);
    }
    if (played) {
        return 1;
    }
#endif
    return 0;
}

/* A size limit avoids loading a music track just to announce a message;
   the load runs in the caller's task, so it also bounds that pause. */
#define TG_NOTIFY_SOUND_FILE_MAX 1048576L
#define TG_NOTIFY_SOUND_TICKS 150UL /* three seconds at DOS's 50 Hz */

static int tg_notify_sound_file_small(const char *path)
{
    struct FileInfoBlock *fib;
    BPTR lock;
    int ok = 0;

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
    if (fib == 0) {
        return 0;
    }
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock != 0) {
        ok = Examine(lock, fib) && fib->fib_DirEntryType < 0 &&
             fib->fib_Size > 0 && fib->fib_Size <= TG_NOTIFY_SOUND_FILE_MAX;
        UnLock(lock);
    }
    FreeDosObject(DOS_FIB, fib);
    return ok;
}

/* The sound.datatype plays on its own once triggered, so the file is
   loaded here, in the caller's task (small, capped, no requesters), and
   tg_notify_sound_poll lets the object go when its end signal arrives or
   three seconds have passed. No second process: DOS packets and the C
   library stay with the one task that already owns them. */
static Object *tg_notify_sound_object = 0;
static const char *tg_notify_sound_reason = "";
static BYTE tg_notify_sound_bit = -1;
static struct DateStamp tg_notify_sound_started;

#if defined(__AROS__)
/* AROS's sound.datatype plays through a mode of its own choosing
   (AHI_BestAudioID), which on a Raspberry Pi is I2S rather than the HDMI
   picked in Prefs/AHI, so there even MultiView is silent. On AROS the
   datatype only reads the file and ahi.device, the unit Prefs/AHI set up,
   plays its sample, as it plays the chime. */
static struct MsgPort *tg_notify_sound_port = 0;
static struct AHIRequest *tg_notify_sound_ahi = 0;

static void tg_notify_sound_ahi_stop(void)
{
    if (tg_notify_sound_ahi != 0) {
        if (!CheckIO((struct IORequest *)tg_notify_sound_ahi)) {
            AbortIO((struct IORequest *)tg_notify_sound_ahi);
        }
        WaitIO((struct IORequest *)tg_notify_sound_ahi);
        CloseDevice((struct IORequest *)tg_notify_sound_ahi);
        tg_notify_chime_request_free(
            (struct IORequest *)tg_notify_sound_ahi);
        tg_notify_sound_ahi = 0;
    }
    if (tg_notify_sound_port != 0) {
        tg_notify_chime_port_free(tg_notify_sound_port);
        tg_notify_sound_port = 0;
    }
}

/* 1 when the object's sample went to ahi.device. */
static int tg_notify_sound_ahi_start(Object *object)
{
    static const ULONG types[4] = { AHIST_M8S, AHIST_S8S, AHIST_M16S,
                                    AHIST_S16S };
    APTR sample = 0;
    IPTR length = 0;
    IPTR rate = 0;
    IPTR type = SDTST_M8S;
    struct AHIRequest *ahi;

    (void)GetDTAttrs(object, SDTA_Sample, (IPTR)&sample,
                     SDTA_SampleLength, (IPTR)&length,
                     SDTA_SamplesPerSec, (IPTR)&rate,
                     SDTA_SampleType, (IPTR)&type, TAG_DONE);
    if (sample == 0 || length == 0 || rate == 0 || type > SDTST_S16S ||
        length > (IPTR)TG_NOTIFY_SOUND_FILE_MAX * 2U) {
        return 0;
    }
    tg_notify_sound_port = tg_notify_chime_port();
    if (tg_notify_sound_port == 0) {
        return 0;
    }
    ahi = (struct AHIRequest *)tg_notify_chime_request(
        tg_notify_sound_port, (ULONG)sizeof(struct AHIRequest));
    if (ahi == 0) {
        tg_notify_sound_ahi_stop();
        return 0;
    }
    ahi->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_DEFAULT_UNIT,
                   (struct IORequest *)ahi, 0) != 0) {
        tg_notify_chime_request_free((struct IORequest *)ahi);
        tg_notify_sound_ahi_stop();
        return 0;
    }
    ahi->ahir_Std.io_Command = CMD_WRITE;
    ahi->ahir_Std.io_Data = sample;
    ahi->ahir_Std.io_Length = (ULONG)(length * SDTM_BYTESPERPOINT(type));
    ahi->ahir_Std.io_Offset = 0;
    ahi->ahir_Type = types[type];
    ahi->ahir_Frequency = (ULONG)rate;
    ahi->ahir_Volume = 0xC000;   /* as loud as the chime */
    ahi->ahir_Position = 0x8000; /* centre */
    ahi->ahir_Link = 0;
    SendIO((struct IORequest *)ahi);
    tg_notify_sound_ahi = ahi;
    return 1;
}
#endif

static void tg_notify_sound_stop(void)
{
    struct dtTrigger trigger;

#if defined(__AROS__)
    tg_notify_sound_ahi_stop(); /* before the object that owns the sample */
#endif
    if (tg_notify_sound_object != 0) {
        trigger.MethodID = DTM_TRIGGER;
        trigger.dtt_GInfo = 0;
        trigger.dtt_Function = STM_STOP;
        trigger.dtt_Data = 0;
        (void)DoDTMethodA(tg_notify_sound_object, 0, 0, (Msg)&trigger);
        DisposeDTObject(tg_notify_sound_object);
        tg_notify_sound_object = 0;
    }
    if (tg_notify_sound_bit >= 0) {
        FreeSignal(tg_notify_sound_bit);
        tg_notify_sound_bit = -1;
    }
#if defined(__amigaos4__)
    if (IDataTypes != 0) {
        DropInterface((struct Interface *)IDataTypes);
        IDataTypes = 0;
    }
#endif
    if (DataTypesBase != 0) {
        CloseLibrary(DataTypesBase);
        DataTypesBase = 0;
    }
}

/* 1 when the file started playing. */
static int tg_notify_sound_start(const char *path)
{
    struct Process *self;
    APTR window;
    struct TagItem tags[6];
#if !defined(__AROS__)
    struct dtTrigger trigger;
#endif
    int played = 0;

    self = (struct Process *)FindTask(0);
    window = self->pr_WindowPtr;
    self->pr_WindowPtr = (APTR)-1; /* no insert-volume requesters */
    if (!tg_notify_sound_file_small(path)) {
        self->pr_WindowPtr = window;
        tg_notify_sound_reason = "missing, a drawer or over 1 MB";
        return 0;
    }
    tg_notify_sound_reason = "datatypes.library V39 did not open";
    DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39L);
#if defined(__amigaos4__)
    if (DataTypesBase != 0) {
        IDataTypes = (struct DataTypesIFace *)GetInterface(
            DataTypesBase, "main", 1L, 0);
    }
    if (IDataTypes != 0)
#else
    if (DataTypesBase != 0)
#endif
    {
        tg_notify_sound_bit = AllocSignal(-1);
    }
    if (tg_notify_sound_bit >= 0) {
        (void)SetSignal(0UL, 1UL << (unsigned int)tg_notify_sound_bit);
        tags[0].ti_Tag = DTA_SourceType;
        tags[0].ti_Data = DTST_FILE;
        tags[1].ti_Tag = DTA_GroupID;
        tags[1].ti_Data = GID_SOUND;
        tags[2].ti_Tag = SDTA_SignalTask;
        tags[2].ti_Data = TG_NOTIFY_TAG(self);
        /* A mask, despite the name: sound.datatype V40 shipped it that way
           and every system kept it (SDTA_SignalBitMask in newer headers). */
        tags[3].ti_Tag = SDTA_SignalBit;
        tags[3].ti_Data = 1UL << (unsigned int)tg_notify_sound_bit;
        tags[4].ti_Tag = SDTA_Cycles;
        tags[4].ti_Data = 1UL;
        tags[5].ti_Tag = TAG_DONE;
        tags[5].ti_Data = 0UL;
        tg_notify_sound_reason = "no sound datatype reads it";
        tg_notify_sound_object = NewDTObjectA((APTR)path, tags);
        if (tg_notify_sound_object != 0) {
#if defined(__AROS__)
            tg_notify_sound_reason = "ahi.device would not play it";
            played = tg_notify_sound_ahi_start(tg_notify_sound_object);
#else
            tg_notify_sound_reason = "the datatype would not play it";
            trigger.MethodID = DTM_TRIGGER;
            trigger.dtt_GInfo = 0;
            trigger.dtt_Function = STM_PLAY;
            trigger.dtt_Data = 0;
            played = DoDTMethodA(tg_notify_sound_object, 0, 0,
                                 (Msg)&trigger) != 0UL;
#endif
        }
    }
    self->pr_WindowPtr = window;
    if (played) {
        DateStamp(&tg_notify_sound_started);
    } else {
        tg_notify_sound_stop();
    }
    return played;
}

void tg_notify_sound_poll(void)
{
    struct DateStamp now;
    ULONG mask;
    long ticks;

    if (tg_notify_sound_object == 0) {
        return;
    }
#if defined(__AROS__)
    if (tg_notify_sound_ahi != 0 &&
        CheckIO((struct IORequest *)tg_notify_sound_ahi)) {
        tg_notify_sound_stop();
        return;
    }
#endif
    mask = 1UL << (unsigned int)tg_notify_sound_bit;
    if ((SetSignal(0UL, 0UL) & mask) != 0UL) {
        (void)SetSignal(0UL, mask);
        tg_notify_sound_stop();
        return;
    }
    DateStamp(&now);
    ticks = (now.ds_Days - tg_notify_sound_started.ds_Days) * 24L * 60L *
                TICKS_PER_SECOND * 60L +
            (now.ds_Minute - tg_notify_sound_started.ds_Minute) * 60L *
                TICKS_PER_SECOND +
            (now.ds_Tick - tg_notify_sound_started.ds_Tick);
    if (ticks < 0L || ticks >= (long)TG_NOTIFY_SOUND_TICKS) {
        tg_notify_sound_stop();
    }
}

int tg_notify_sound_play(const char *path)
{
    if (path == 0) {
        return TG_NOTIFY_HEARD_NOTHING;
    }
    tg_notify_sound_stop(); /* a new message cuts the last sound short */
    tg_notify_sound_reason = "";
    if (path[0] != '\0' && tg_notify_sound_start(path)) {
        return TG_NOTIFY_HEARD_FILE;
    }
    if (tg_notify_chime_play()) {
        return TG_NOTIFY_HEARD_CHIME;
    }
    tg_platform_display_beep();
    return TG_NOTIFY_HEARD_BEEP;
}

const char *tg_notify_sound_why(void)
{
    return tg_notify_sound_reason;
}

void tg_notify_sound_shutdown(void)
{
    tg_notify_sound_stop();
}

#else

/* The 68000 text build has no datatype state or sample buffer. */
int tg_notify_sound_play(const char *path)
{
    if (path == 0) {
        return TG_NOTIFY_HEARD_NOTHING;
    }
    tg_platform_display_beep();
    return TG_NOTIFY_HEARD_BEEP;
}

const char *tg_notify_sound_why(void)
{
    return "";
}

void tg_notify_sound_poll(void)
{
}

void tg_notify_sound_shutdown(void)
{
}

#endif

