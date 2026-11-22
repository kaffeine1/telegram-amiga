/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The login QR code of the text client. On an Amiga a console line holds
   one module at best and a code needs about forty lines, more than a
   256-line Workbench gives a console, so the code goes in a small window
   of its own on the Workbench screen; the console keeps the keyboard (the
   window never activates) and goes on taking P for the phone number. The
   host build prints it in the terminal with block characters. */

#include <stdio.h>
#include <string.h>

#include "tg_qr.h"

#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__AROS__)
#define TG_QR_VIEW_AMIGA 1
#endif

#if defined(TG_QR_VIEW_AMIGA)

#if defined(__amigaos4__)
#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif
#endif
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

/* intuition.library's base belongs to the platform file, graphics.library's
   to the window (tg_gui_window.c), which the text-only 68000 build leaves
   out: there it lives here. The text client runs without the window, so
   whatever is not open yet is opened here and closed again with the code. */
#if defined(__amigaos4__)
extern struct Library *IntuitionBase;
extern struct IntuitionIFace *IIntuition;
extern struct Library *GfxBase;
extern struct GraphicsIFace *IGraphics;
#else
extern struct IntuitionBase *IntuitionBase;
#if defined(TG_NO_GUI)
struct GfxBase *GfxBase = 0;
#else
extern struct GfxBase *GfxBase;
#endif
#endif

#if defined(__AROS__)
#define TG_QR_TAG(v) ((IPTR)(v))
#else
#define TG_QR_TAG(v) ((ULONG)(v))
#endif

static struct Window *tg_qr_view_win;
static struct ColorMap *tg_qr_view_cmap;
static LONG tg_qr_view_pen_dark = -1;
static LONG tg_qr_view_pen_light = -1;
static int tg_qr_view_size;
static int tg_qr_view_module_w;
static int tg_qr_view_module_h;
static int tg_qr_view_quiet;
static int tg_qr_view_own_intuition;
static int tg_qr_view_own_gfx;
#if defined(__amigaos4__)
static int tg_qr_view_own_iintuition;
static int tg_qr_view_own_igfx;
#endif

static int tg_qr_view_libs(void)
{
#if defined(__amigaos4__)
    if (IntuitionBase == 0) {
        IntuitionBase = OpenLibrary("intuition.library", 50L);
        tg_qr_view_own_intuition = IntuitionBase != 0;
    }
    if (IntuitionBase != 0 && IIntuition == 0) {
        IIntuition = (struct IntuitionIFace *)GetInterface(IntuitionBase,
                                                           "main", 1L, 0);
        tg_qr_view_own_iintuition = IIntuition != 0;
    }
    if (GfxBase == 0) {
        GfxBase = OpenLibrary("graphics.library", 50L);
        tg_qr_view_own_gfx = GfxBase != 0;
    }
    if (GfxBase != 0 && IGraphics == 0) {
        IGraphics = (struct GraphicsIFace *)GetInterface(GfxBase, "main", 1L,
                                                         0);
        tg_qr_view_own_igfx = IGraphics != 0;
    }
    return IIntuition != 0 && IGraphics != 0;
#else
    if (IntuitionBase == 0) {
        IntuitionBase = (struct IntuitionBase *)OpenLibrary(
            (CONST_STRPTR)"intuition.library", 39L);
        tg_qr_view_own_intuition = IntuitionBase != 0;
    }
    if (GfxBase == 0) {
        GfxBase = (struct GfxBase *)OpenLibrary(
            (CONST_STRPTR)"graphics.library", 39L);
        tg_qr_view_own_gfx = GfxBase != 0;
    }
    return IntuitionBase != 0 && GfxBase != 0;
#endif
}

static void tg_qr_view_libs_close(void)
{
#if defined(__amigaos4__)
    if (tg_qr_view_own_igfx) {
        DropInterface((struct Interface *)IGraphics);
        IGraphics = 0;
        tg_qr_view_own_igfx = 0;
    }
    if (tg_qr_view_own_iintuition) {
        DropInterface((struct Interface *)IIntuition);
        IIntuition = 0;
        tg_qr_view_own_iintuition = 0;
    }
#endif
    if (tg_qr_view_own_gfx) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = 0;
        tg_qr_view_own_gfx = 0;
    }
    if (tg_qr_view_own_intuition) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
        tg_qr_view_own_intuition = 0;
    }
}

/* Pure black and pure white from the screen's own palette: the nearest it
   has on a paletted screen, the exact colour on a truecolour one. */
static LONG tg_qr_view_pen(ULONG level)
{
    struct TagItem tags[2];

    tags[0].ti_Tag = OBP_Precision;
    tags[0].ti_Data = PRECISION_EXACT;
    tags[1].ti_Tag = TAG_END;
    tags[1].ti_Data = 0;
    return ObtainBestPenA(tg_qr_view_cmap, level, level, level, tags);
}

static void tg_qr_view_draw(const tg_qr_code *code)
{
    struct RastPort *rp = tg_qr_view_win->RPort;
    int mw = tg_qr_view_module_w;
    int mh = tg_qr_view_module_h;
    int q = tg_qr_view_quiet;
    int ox = tg_qr_view_win->BorderLeft;
    int oy = tg_qr_view_win->BorderTop;
    int side_w = mw * (code->size + 2 * q);
    int side_h = mh * (code->size + 2 * q);
    int x;
    int y;

    SetAPen(rp, (ULONG)(tg_qr_view_pen_light >= 0 ? tg_qr_view_pen_light : 2));
    RectFill(rp, ox, oy, ox + side_w - 1, oy + side_h - 1);
    SetAPen(rp, (ULONG)(tg_qr_view_pen_dark >= 0 ? tg_qr_view_pen_dark : 1));
    for (y = 0; y < code->size; ++y) {
        x = 0;
        while (x < code->size) {
            int run = 0;

            while (x + run < code->size && tg_qr_dark(code, x + run, y)) {
                ++run;
            }
            if (run > 0) {
                RectFill(rp, ox + (x + q) * mw, oy + (y + q) * mh,
                         ox + (x + run + q) * mw - 1,
                         oy + (y + q + 1) * mh - 1);
                x += run;
            } else {
                ++x;
            }
        }
    }
}

/* How much taller than wide the screen's pixels are, times ten (20 on a
   hires screen without interlace), from the display database, for the
   chipset's own modes only (the monitors up to DblPAL): an RTG driver
   reckons for a 4:3 monitor whatever the panel, and its pixels are
   square, so anything else is 10. */
static int tg_qr_view_ratio(struct Screen *scr)
{
    struct DisplayInfo info;
    ULONG mode = GetVPModeID(&scr->ViewPort);
    long ratio = 10L;

    if (mode != (ULONG)INVALID_ID &&
        (mode & 0xFFFF1000UL) <= 0x000A1000UL && /* PAL, NTSC ... DblPAL */
        GetDisplayInfoData(0, (UBYTE *)&info, sizeof(info), DTAG_DISP,
                           mode) > 0 &&
        info.Resolution.x > 0 && info.Resolution.y > 0) {
        ratio = (10L * (long)info.Resolution.y + (long)info.Resolution.x / 2L) /
                (long)info.Resolution.x;
    }
    return ratio < 5L ? 5 : ratio > 40L ? 40 : (int)ratio;
}

/* The module's width and height in pixels for a code of `cells` a side in
   half the screen: as wide as fits, 2 to 6, and as tall as that shows. */
static void tg_qr_view_modules(struct Screen *scr, int cells, int ratio,
                               int *mw, int *mh)
{
    int w;
    int h = 1;

    for (w = 6; w >= 2; --w) {
        h = (w * 10 + ratio / 2) / ratio;
        if (h < 1) {
            h = 1;
        }
        if (w == 2 || (w * cells <= scr->Width / 2 &&
                       h * cells <= scr->Height / 2)) {
            break;
        }
    }
    *mw = w;
    *mh = h;
}

static int tg_qr_view_open(int size)
{
    struct Screen *scr;
    struct TagItem tags[13];
    int ratio;
    int mw;
    int mh;
    int side_w;
    int side_h;
    int n = 0;

    scr = LockPubScreen(0);
    if (scr == 0) {
        return 2;
    }
    ratio = tg_qr_view_ratio(scr);
    tg_qr_view_quiet = 4;
    tg_qr_view_modules(scr, size + 8, ratio, &mw, &mh);
    if (mw < 3) {
        tg_qr_view_quiet = 2; /* a small screen: half the margin */
        tg_qr_view_modules(scr, size + 4, ratio, &mw, &mh);
    }
    side_w = mw * (size + 2 * tg_qr_view_quiet);
    side_h = mh * (size + 2 * tg_qr_view_quiet);
    tags[n].ti_Tag = WA_PubScreen;
    tags[n++].ti_Data = TG_QR_TAG(scr);
    tags[n].ti_Tag = WA_Left;
    tags[n++].ti_Data = TG_QR_TAG(scr->Width > side_w + 40
                                      ? scr->Width - side_w - 40 : 0);
    tags[n].ti_Tag = WA_Top;
    tags[n++].ti_Data = TG_QR_TAG(scr->BarHeight + 8);
    tags[n].ti_Tag = WA_InnerWidth;
    tags[n++].ti_Data = TG_QR_TAG(side_w);
    tags[n].ti_Tag = WA_InnerHeight;
    tags[n++].ti_Data = TG_QR_TAG(side_h);
    tags[n].ti_Tag = WA_Title;
    tags[n++].ti_Data = TG_QR_TAG("Amigram: scan with Telegram");
    tags[n].ti_Tag = WA_DragBar;
    tags[n++].ti_Data = TG_QR_TAG(TRUE);
    tags[n].ti_Tag = WA_DepthGadget;
    tags[n++].ti_Data = TG_QR_TAG(TRUE);
    tags[n].ti_Tag = WA_Activate;
    tags[n++].ti_Data = TG_QR_TAG(FALSE); /* the console keeps the keys */
    tags[n].ti_Tag = WA_SmartRefresh;
    tags[n++].ti_Data = TG_QR_TAG(TRUE);
    tags[n].ti_Tag = WA_NoCareRefresh;
    tags[n++].ti_Data = TG_QR_TAG(TRUE);
    tags[n].ti_Tag = WA_RMBTrap;
    tags[n++].ti_Data = TG_QR_TAG(TRUE);
    tags[n].ti_Tag = TAG_END;
    tags[n].ti_Data = 0;
    tg_qr_view_win = OpenWindowTagList(0, tags);
    if (tg_qr_view_win != 0) {
        tg_qr_view_cmap = scr->ViewPort.ColorMap;
        tg_qr_view_pen_dark = tg_qr_view_pen(0x00000000UL);
        tg_qr_view_pen_light = tg_qr_view_pen(0xffffffffUL);
        tg_qr_view_size = size;
        tg_qr_view_module_w = mw;
        tg_qr_view_module_h = mh;
    }
    UnlockPubScreen(0, scr);
    return tg_qr_view_win != 0 ? 0 : 2;
}

int tg_qr_view_show(const tg_qr_code *code, FILE *stream)
{
    (void)stream;
    if (code == 0 || code->size <= 0) {
        return 2;
    }
    if (tg_qr_view_win != 0 && tg_qr_view_size != code->size) {
        tg_qr_view_close(); /* a longer token: a bigger code */
    }
    if (tg_qr_view_win == 0) {
        if (!tg_qr_view_libs() || tg_qr_view_open(code->size) != 0) {
            tg_qr_view_libs_close();
            return 2;
        }
    }
    tg_qr_view_draw(code);
    return 0;
}

void tg_qr_view_close(void)
{
    if (tg_qr_view_win != 0) {
        if (tg_qr_view_pen_dark >= 0) {
            ReleasePen(tg_qr_view_cmap, (ULONG)tg_qr_view_pen_dark);
        }
        if (tg_qr_view_pen_light >= 0) {
            ReleasePen(tg_qr_view_cmap, (ULONG)tg_qr_view_pen_light);
        }
        tg_qr_view_pen_dark = -1;
        tg_qr_view_pen_light = -1;
        CloseWindow(tg_qr_view_win);
        tg_qr_view_win = 0;
    }
    tg_qr_view_libs_close();
}

#else /* the host build: the terminal, two rows of modules a line */

int tg_qr_view_show(const tg_qr_code *code, FILE *stream)
{
    int x;
    int y;

    if (code == 0 || stream == 0 || code->size <= 0) {
        return 2;
    }
    fputc('\n', stream);
    for (y = -2; y < code->size + 2; y += 2) {
        fputs("    \033[30;47m", stream);
        for (x = -2; x < code->size + 2; ++x) {
            int top = tg_qr_dark(code, x, y);
            int bottom = tg_qr_dark(code, x, y + 1);

            fputs(top && bottom ? "\xe2\x96\x88"
                  : top         ? "\xe2\x96\x80"
                  : bottom      ? "\xe2\x96\x84"
                                : " ",
                  stream);
        }
        fputs("\033[0m\n", stream);
    }
    fputc('\n', stream);
    fflush(stream);
    return 0;
}

void tg_qr_view_close(void)
{
}

#endif
