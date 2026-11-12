/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * Author: Michele Dipace (@kaffeine1)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* A picture read through the system's datatypes: for the formats this
   program has no decoder of its own for. A sticker is a WEBP, and a
   machine with a WEBP datatype installed can show it; one without keeps
   the sticker's emoji. Nothing here knows the format: the system does. */

#include "tg_avatar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__amigaos3__) || defined(__amigaos4__) || \
     defined(__MORPHOS__) || defined(__AROS__)) && !defined(TG_LOWMEM) && \
    !defined(TG_NO_GUI)

#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif
#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#if defined(__amigaos4__)
#include <proto/intuition.h>
#else
#include <clib/alib_protos.h>
#endif

#if defined(__AROS__)
#define TG_DT_TAG(p) ((IPTR)(p))
#else
#define TG_DT_TAG(p) ((ULONG)(p))
#endif

/* The true-colour side of picture.datatype (V43): older headers lack it. */
#ifndef PDTA_DestMode
#define PDTA_DestMode (DTA_Dummy + 251)
#endif
#ifndef PMODE_V43
#define PMODE_V43 1
#endif
#ifndef PDTM_READPIXELARRAY
#define PDTM_READPIXELARRAY (DTM_Dummy + 0x61)
#endif
#ifndef PBPAFMT_ARGB
#define PBPAFMT_ARGB 2
#endif
#ifndef mskHasAlpha
#define mskHasAlpha 4
#endif

/* tg_notify_sound.c owns the library base, open only while it plays a
   sound. A picture is read in one call, between two of its turns: the
   base is borrowed when it is open, opened and closed here when not. */
extern struct Library *DataTypesBase;
#if defined(__amigaos4__)
extern struct DataTypesIFace *IDataTypes;
#endif

static int tg_image_dt_open(int *opened)
{
    *opened = 0;
#if defined(__amigaos4__)
    if (DataTypesBase != 0 && IDataTypes != 0) {
        return 1;
    }
#else
    if (DataTypesBase != 0) {
        return 1;
    }
#endif
    DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39L);
    if (DataTypesBase == 0) {
        return 0;
    }
#if defined(__amigaos4__)
    IDataTypes = (struct DataTypesIFace *)GetInterface(DataTypesBase, "main",
                                                      1L, 0);
    if (IDataTypes == 0) {
        CloseLibrary(DataTypesBase);
        DataTypesBase = 0;
        return 0;
    }
#endif
    *opened = 1;
    return 1;
}

static void tg_image_dt_close(int opened)
{
    if (!opened) {
        return;
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

/* A method sent straight to the object. DoDTMethodA() is for the methods
   that carry a GadgetInfo: it writes one into the second field of any
   message, which in a pdtBlitPixelArray is the address of the pixels. */
static ULONG tg_image_dt_method(Object *object, Msg message)
{
#if defined(__amigaos4__)
    return (ULONG)IDoMethodA(object, message);
#else
    return (ULONG)DoMethodA(object, message);
#endif
}

/* The bytes put at the head of every row before the picture is asked
   for: a row that still has them was not written. */
#define TG_IMAGE_DT_MARK 8UL

static unsigned char tg_image_dt_mark(unsigned long k)
{
    return (unsigned char)((k * 7UL) + 13UL);
}

/* The whole picture as ARGB into `argb`, in one request. That is the one
   way of asking that every picture.datatype from V43 on answers alike:
   the one of AfA OS (46.9), found on a real machine, answers 1 to a
   request for RGBA and writes nothing, and hands back scrambled rows when
   asked for a part of the picture. So the answer is not looked at either
   (it is 1 on some systems, the number of pixels on others): the picture
   counts as read when every row has lost the bytes put at its head. */
static int tg_image_dt_read_all(Object *object, unsigned char *argb, int w,
                                int h)
{
    /* the system's own message: on 64-bit AROS its members are wider */
    struct pdtBlitPixelArray read;
    unsigned long row_bytes = (unsigned long)w * 4UL;
    unsigned long mark = row_bytes < TG_IMAGE_DT_MARK ? row_bytes
                                                      : TG_IMAGE_DT_MARK;
    unsigned long k;
    int y;

    for (y = 0; y < h; ++y) {
        unsigned char *row = argb + ((unsigned long)y * row_bytes);

        for (k = 0UL; k < mark; ++k) {
            row[k] = tg_image_dt_mark(((unsigned long)y * 3UL) + k);
        }
    }
    memset(&read, 0, sizeof(read));
    read.MethodID = PDTM_READPIXELARRAY;
    read.pbpa_PixelData = argb;
    read.pbpa_PixelFormat = PBPAFMT_ARGB;
    read.pbpa_PixelArrayMod = (ULONG)row_bytes;
    read.pbpa_Left = 0UL;
    read.pbpa_Top = 0UL;
    read.pbpa_Width = (ULONG)w;
    read.pbpa_Height = (ULONG)h;
    (void)tg_image_dt_method(object, (Msg)&read);
    for (y = 0; y < h; ++y) {
        const unsigned char *row = argb + ((unsigned long)y * row_bytes);

        for (k = 0UL; k < mark; ++k) {
            if (row[k] != tg_image_dt_mark(((unsigned long)y * 3UL) + k)) {
                break;
            }
        }
        if (k == mark) {
            return 0; /* this row was not written: a picture.datatype
                         older than V43 keeps no such pixels */
        }
    }
    return 1;
}

int tg_image_dt_decode_rgb(const char *path, unsigned long background,
                           unsigned char **rgb_out, int *w_out, int *h_out)
{
    struct Process *self;
    APTR window;
    struct TagItem tags[6];
    struct TagItem get[2];
    struct BitMapHeader *header = 0;
    Object *object;
    unsigned char *pixels = 0;
    unsigned char bg[3];
    unsigned long count;
    unsigned long i;
    int opened;
    int use_alpha;
    int w;
    int h;
    int rc = TG_IMAGE_DT_NO_DATATYPE;

    if (rgb_out != 0) {
        *rgb_out = 0;
    }
    if (path == 0 || rgb_out == 0 || w_out == 0 || h_out == 0) {
        return TG_IMAGE_DT_FAILED;
    }
    bg[0] = (unsigned char)((background >> 16) & 0xffUL);
    bg[1] = (unsigned char)((background >> 8) & 0xffUL);
    bg[2] = (unsigned char)(background & 0xffUL);
    if (!tg_image_dt_open(&opened)) {
        return TG_IMAGE_DT_NO_DATATYPE;
    }
    self = (struct Process *)FindTask(0);
    window = self->pr_WindowPtr;
    self->pr_WindowPtr = (APTR)-1; /* no requesters from a datatype */
    /* The picture as the datatype delivers it: nothing is remapped to a
       screen, so none is needed and no layout is asked for. */
    tags[0].ti_Tag = DTA_SourceType;
    tags[0].ti_Data = DTST_FILE;
    tags[1].ti_Tag = DTA_GroupID;
    tags[1].ti_Data = GID_PICTURE;
    tags[2].ti_Tag = PDTA_Remap;
    tags[2].ti_Data = FALSE;
    tags[3].ti_Tag = PDTA_DestMode;
    tags[3].ti_Data = PMODE_V43;
    tags[4].ti_Tag = TAG_DONE;
    tags[4].ti_Data = 0UL;
    object = NewDTObjectA((APTR)path, tags);
    if (object != 0) {
        rc = TG_IMAGE_DT_FAILED;
        get[0].ti_Tag = PDTA_BitMapHeader;
        get[0].ti_Data = TG_DT_TAG(&header);
        get[1].ti_Tag = TAG_DONE;
        get[1].ti_Data = 0UL;
        (void)GetDTAttrsA(object, get);
        w = header != 0 ? (int)header->bmh_Width : 0;
        h = header != 0 ? (int)header->bmh_Height : 0;
        if (w > 0 && h > 0 && w <= TG_IMAGE_DT_EDGE_MAX &&
            h <= TG_IMAGE_DT_EDGE_MAX) {
            count = (unsigned long)w * (unsigned long)h;
            use_alpha = header->bmh_Masking == mskHasAlpha;
            /* one block: the four bytes of a pixel as they come, the
               three that are kept written over them from the front */
            pixels = (unsigned char *)malloc((size_t)count * 4U);
            if (pixels == 0) {
                rc = TG_IMAGE_DT_NO_MEMORY;
            } else if (tg_image_dt_read_all(object, pixels, w, h)) {
                rc = 0;
            }
            if (rc == 0 && use_alpha) {
                /* Alpha promised and not delivered would make every
                   pixel the background: the colours are there, so the
                   picture is then taken as an opaque one. */
                for (i = 0UL; i < count && pixels[i * 4UL] == 0U; ++i) {
                }
                if (i == count) {
                    use_alpha = 0;
                }
            }
            for (i = 0UL; rc == 0 && i < count; ++i) {
                /* alpha, red, green, blue */
                const unsigned char *p = pixels + (i * 4UL);
                unsigned int a = use_alpha ? p[0] : 255U;
                unsigned int red = p[1];
                unsigned int green = p[2];
                unsigned int blue = p[3];
                unsigned char *dst = pixels + (i * 3UL);

                dst[0] = (unsigned char)(
                    ((red * a) + (bg[0] * (255U - a)) + 127U) / 255U);
                dst[1] = (unsigned char)(
                    ((green * a) + (bg[1] * (255U - a)) + 127U) / 255U);
                dst[2] = (unsigned char)(
                    ((blue * a) + (bg[2] * (255U - a)) + 127U) / 255U);
            }
        }
        DisposeDTObject(object);
        if (rc == 0) {
            *rgb_out = pixels;
            *w_out = w;
            *h_out = h;
            pixels = 0;
        }
    }
    self->pr_WindowPtr = window;
    free(pixels);
    tg_image_dt_close(opened);
    return rc;
}

/* The smallest WEBP there is, two by two pixels, all of one red (200, 30,
   30): what a machine is asked to open, once, to learn whether it has a
   datatype for the format and whether the pixels that come back are the
   picture's. */
static const unsigned char tg_image_dt_webp_probe[] = {
    0x52, 0x49, 0x46, 0x46, 0x1e, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x4c, 0x11, 0x00, 0x00, 0x00, 0x2f, 0x01, 0x40, 0x00,
    0x00, 0x07, 0x50, 0x8f, 0x22, 0xd7, 0xa3, 0xff, 0x81, 0x88, 0xe8, 0x7f,
    0x00, 0x00
};

/* -1 = not asked yet */
static int tg_image_dt_webp_known = -1;

/* The four pixels of the probe as they must come back: a size alone says
   nothing about what is in the buffer. A little room is left for a system
   that keeps its pictures in fewer bits. */
static int tg_image_dt_is_probe_red(const unsigned char *rgb)
{
    static const int want[3] = { 200, 30, 30 };
    int k;

    for (k = 0; k < 12; ++k) {
        int d = (int)rgb[k] - want[k % 3];

        if (d < -24 || d > 24) {
            return 0;
        }
    }
    return 1;
}

int tg_image_dt_reads_webp(void)
{
    static const char probe[] = "data/tg-webp-probe.webp";
    unsigned char *rgb = 0;
    FILE *f;
    int w = 0;
    int h = 0;

    if (tg_image_dt_webp_known >= 0) {
        return tg_image_dt_webp_known;
    }
    tg_image_dt_webp_known = 0;
    f = fopen(probe, "wb");
    if (f == 0) {
        return 0;
    }
    w = fwrite(tg_image_dt_webp_probe, 1, sizeof(tg_image_dt_webp_probe),
               f) == sizeof(tg_image_dt_webp_probe);
    if (fclose(f) == 0 && w) {
        tg_image_dt_webp_known =
            tg_image_dt_decode_rgb(probe, 0UL, &rgb, &w, &h) == 0 &&
            w == 2 && h == 2 && tg_image_dt_is_probe_red(rgb);
        free(rgb);
    }
    (void)remove(probe);
    return tg_image_dt_webp_known;
}

int tg_image_dt_truecolor(void)
{
    /* opened by its version alone: nothing of it is called from here */
    struct Library *picture =
        OpenLibrary((CONST_STRPTR)"datatypes/picture.datatype", 43L);

    if (picture == 0) {
        return 0;
    }
    CloseLibrary(picture);
    return 1;
}

int tg_image_dt_refresh(void)
{
    struct TagItem tags[3];
    BPTR nil_in;
    BPTR nil_out;
    LONG rc = -1L;

    tg_image_dt_webp_known = -1; /* whatever follows, the machine is asked
                                    again */
    nil_in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    nil_out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (nil_in != 0 && nil_out != 0) {
        /* The system's own command reads the descriptors in DEVS:DataTypes
           again: datatypes.library has no call for it. */
        tags[0].ti_Tag = SYS_Input;
        tags[0].ti_Data = TG_DT_TAG(nil_in);
        tags[1].ti_Tag = SYS_Output;
        tags[1].ti_Data = TG_DT_TAG(nil_out);
        tags[2].ti_Tag = TAG_DONE;
        tags[2].ti_Data = 0UL;
        rc = SystemTagList((CONST_STRPTR)"AddDataTypes REFRESH", tags);
    }
    if (nil_in != 0) {
        Close(nil_in);
    }
    if (nil_out != 0) {
        Close(nil_out);
    }
    return rc == 0L ? 0 : 1;
}

#else /* no datatypes here: the host, the text-only builds */

int tg_image_dt_decode_rgb(const char *path, unsigned long background,
                           unsigned char **rgb_out, int *w_out, int *h_out)
{
    (void)path;
    (void)background;
    (void)w_out;
    (void)h_out;
    if (rgb_out != 0) {
        *rgb_out = 0;
    }
    return TG_IMAGE_DT_NO_DATATYPE;
}

int tg_image_dt_reads_webp(void)
{
    return 0;
}

int tg_image_dt_truecolor(void)
{
    return 0;
}

int tg_image_dt_refresh(void)
{
    return 1;
}

#endif
