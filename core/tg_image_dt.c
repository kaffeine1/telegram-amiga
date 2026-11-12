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
#ifndef PBPAFMT_RGBA
#define PBPAFMT_RGBA 1
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

/* One row of the picture as RGBA into `row`. What the method answers
   differs between the systems (1, the number of pixels, and on some
   nothing that can be trusted), so a row counts as read when the answer
   is not zero or when the bytes put there beforehand are gone. */
static int tg_image_dt_read_row(Object *object, unsigned char *row, int w,
                                int y)
{
    /* the system's own message: on 64-bit AROS its members are wider */
    struct pdtBlitPixelArray read;
    int bytes = w * 4;
    int k;

    for (k = 0; k < bytes; ++k) {
        row[k] = (unsigned char)((k * 7) + 13);
    }
    memset(&read, 0, sizeof(read));
    read.MethodID = PDTM_READPIXELARRAY;
    read.pbpa_PixelData = row;
    read.pbpa_PixelFormat = PBPAFMT_RGBA;
    read.pbpa_PixelArrayMod = (ULONG)bytes;
    read.pbpa_Left = 0UL;
    read.pbpa_Top = (ULONG)y;
    read.pbpa_Width = (ULONG)w;
    read.pbpa_Height = 1UL;
    if (tg_image_dt_method(object, (Msg)&read) != 0UL) {
        return 1;
    }
    for (k = 0; k < bytes; ++k) {
        if (row[k] != (unsigned char)((k * 7) + 13)) {
            return 1;
        }
    }
    return 0; /* a picture.datatype older than V43 keeps no such pixels */
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
    unsigned char *rgb = 0;
    unsigned char *row = 0;
    unsigned char bg[3];
    unsigned long any_alpha = 0UL;
    int opened;
    int use_alpha;
    int pass;
    int w;
    int h;
    int x;
    int y;
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
            use_alpha = header->bmh_Masking == mskHasAlpha;
            rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3U);
            row = (unsigned char *)malloc((size_t)w * 4U);
            if (rgb == 0 || row == 0) {
                rc = TG_IMAGE_DT_NO_MEMORY;
            } else {
                rc = 0;
            }
            /* Alpha promised and not delivered makes every pixel the
               background: the colours are there, so the picture is then
               read a second time as an opaque one. */
            for (pass = 0; pass < 2 && rc == 0; ++pass) {
                any_alpha = 0UL;
                for (y = 0; y < h; ++y) {
                    unsigned char *dst = rgb + ((size_t)y * (size_t)w * 3U);

                    if (!tg_image_dt_read_row(object, row, w, y)) {
                        rc = TG_IMAGE_DT_FAILED;
                        break;
                    }
                    for (x = 0; x < w; ++x) {
                        const unsigned char *p = row + ((size_t)x * 4U);
                        unsigned int a = use_alpha ? p[3] : 255U;
                        int c;

                        any_alpha |= a;
                        for (c = 0; c < 3; ++c) {
                            dst[(x * 3) + c] = (unsigned char)(
                                ((p[c] * a) + (bg[c] * (255U - a)) + 127U) /
                                255U);
                        }
                    }
                }
                if (any_alpha != 0UL) {
                    break;
                }
                use_alpha = 0;
            }
        }
        DisposeDTObject(object);
        if (rc == 0) {
            *rgb_out = rgb;
            *w_out = w;
            *h_out = h;
            rgb = 0;
        }
    }
    self->pr_WindowPtr = window;
    free(row);
    free(rgb);
    tg_image_dt_close(opened);
    return rc;
}

/* The smallest WEBP there is, two by two pixels: what a machine is asked
   to open, once, to learn whether it has a datatype for the format. */
static const unsigned char tg_image_dt_webp_probe[] = {
    0x52, 0x49, 0x46, 0x46, 0x1e, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x4c, 0x11, 0x00, 0x00, 0x00, 0x2f, 0x01, 0x40, 0x00,
    0x00, 0x07, 0x50, 0x8f, 0x22, 0xd7, 0xa3, 0xff, 0x81, 0x88, 0xe8, 0x7f,
    0x00, 0x00
};

/* -1 = not asked yet */
static int tg_image_dt_webp_known = -1;

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
            w == 2 && h == 2;
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
