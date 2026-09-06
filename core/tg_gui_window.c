/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 *
 * Phase 5b milestone 0: the native window backend behind tg_gui_backend.
 * One file, two branches -- the OS4 interface model and the classic shared
 * base model (OS3 / MorphOS / AROS) differ only in how libraries are opened;
 * every draw call (RectFill, Text, TextLength) is source-identical because the
 * portable renderer in tg_gui.c does all the layout. The host build (no Amiga
 * platform macro) compiles only the stub at the bottom.
 *
 * The window is painted entirely by tg_gui_paint(); this file is the thin
 * backend (window + RastPort + theme pens + event loop) plus a redraw-time and
 * footprint measurement, which is the whole point of milestone 0 on a 68k.
 */

#include "tg_gui.h"
#include "tg_gui_session.h"
#include "tg_avatar.h"
#include "tg_emoji_sheet.h"
#include "tg_mtproto_login.h"
#include "tg_platform.h"
#include "tg_version.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__AROS__)
#define TG_GUI_AMIGA 1
#endif

/* While the composer is focused, defer the (blocking) live network poll until
   typing has paused this many seconds, so a quiet recv never stalls active
   keystrokes -- yet live messages and the "is typing" header still arrive as
   soon as you pause. Without this the poll was skipped for the whole time the
   composer was focused, and "keep composer focus after send" leaves it focused,
   so reception + the typing header silently stopped until a chat switch. */
#define TG_GUI_COMPOSE_IDLE_POLL_SECONDS 3UL
/* While keys are flowing, drain at most one already-queued MTProto frame each
   second. This path sends no RPC and therefore cannot leave a reply pending. */
#define TG_GUI_COMPOSE_RECEIVE_SECONDS 1UL
/* While a file transfer is pumping, the FULL live tick (a blocking
   getHistory, ~half a second on a 68080) is throttled to this cadence and
   the light receive_pending drain covers incoming pushes in between --
   otherwise the 2s tick stole ~25% of the transfer AND froze the UI in
   half-second bites (the "slow while downloading" Vampire report). */
#define TG_GUI_TRANSFER_POLL_SECONDS 10UL

#if defined(TG_GUI_AMIGA)

/* AmigaOS4: make the library calls expand to interface inlines (IGraphics->...,
   IIntuition->...) instead of unresolved external symbols, exactly as the OS4
   platform file does. Harmless on the classic-base targets. */
#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <intuition/intuition.h>
#include <devices/timer.h>
#include <devices/clipboard.h>
#include <libraries/asl.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/wb.h>
#include <proto/icon.h>
#include <proto/asl.h>

/* asl.library is opened lazily around the file requester (Send file...) so
   startup stays untouched and a system without it just skips the feature. */
struct Library *AslBase = 0;
/* workbench.library + icon.library open lazily around the iconified wait. */
struct Library *WorkbenchBase = 0;
struct Library *IconBase = 0;
#if defined(__amigaos4__)
struct WorkbenchIFace *IWorkbench = 0;
struct IconIFace *IIcon = 0;
struct AslIFace *IAsl = 0;
#endif

/* timer.device request, papering over the OS4 SDK rename (TimeRequest with
   Request/Time.Seconds vs the classic timerequest with tr_node/tr_time). */
#if defined(__amigaos4__)
typedef struct TimeRequest tg_gui_timereq;
#define TG_GUI_TR_NODE(t) ((t)->Request)
#define TG_GUI_TR_SECS(t) ((t)->Time.Seconds)
#define TG_GUI_TR_MICRO(t) ((t)->Time.Microseconds)
#else
typedef struct timerequest tg_gui_timereq;
#define TG_GUI_TR_NODE(t) ((t)->tr_node)
#define TG_GUI_TR_SECS(t) ((t)->tr_time.tv_secs)
#define TG_GUI_TR_MICRO(t) ((t)->tr_time.tv_micro)
#endif
/* Heartbeat period: half the fastest watch cadence so a poll window is never
   missed by more than one beat. */
#define TG_GUI_HEARTBEAT_SECS 2UL
#define TG_GUI_PHOTO_FAST_WAKE_US 150000UL
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfx.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <utility/tagitem.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/dos.h>

/* OS4 keeps the FileInfoBlock API behind explicit OBSOLETE* interface
   entries. Its own <dos/obsolete.h> maps the classic names onto them, which
   beats redefining them here (javierdlr, PR #12); the other lanes expose the
   original names directly and need no header at all. */
#if defined(__amigaos4__)
#include <dos/obsolete.h>
#endif
#define TG_GUI_DOS_EXAMINE(lock, fib) Examine((lock), (fib))
#define TG_GUI_DOS_EXNEXT(lock, fib) ExNext((lock), (fib))

/* The modern lanes ship cybergraphics.library headers as part of their SDK.
   Classic OS3 deliberately stays vendor-header-free, but uses the same
   optional RGB888 path through the documented m68k library vectors below. */
#if defined(__amigaos3__)
#define TG_GUI_HAVE_CYBERGRAPHICS 1
#define TG_GUI_CGX_CLASSIC_RUNTIME 1
#define CYBERGFXNAME "cybergraphics.library"
#define CYBRMATTR_DEPTH 0x80000007UL
#define CYBRMATTR_ISCYBERGFX 0x80000008UL
#define RECTFMT_RGB 0UL
#elif defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__MORPHOS) || defined(__AROS__)
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#define TG_GUI_HAVE_CYBERGRAPHICS 1
#endif

/* TagItem ti_Data is pointer-sized. Only AROS needs IPTR (64-bit on AROS
   x86_64, where a ULONG cast would truncate the pointer); the 32-bit targets
   (OS3, MorphOS, OS4 PPC) hold a pointer in a ULONG and do not all expose
   IPTR here. */
#if defined(__AROS__)
#define TG_GUI_TAG(p) ((IPTR)(p))
#else
#define TG_GUI_TAG(p) ((ULONG)(p))
#endif

/* graphics.library is not opened by the C startup or by any platform file, so
   this translation unit defines and owns its base. intuition.library's global
   is defined by the platform file, but this window owner holds its open
   reference for the GUI lifetime. OS4 pairs each base with an interface. */
#if defined(__amigaos4__)
struct Library *GfxBase = 0;
struct GraphicsIFace *IGraphics = 0;
struct Library *GadToolsBase = 0;
struct GadToolsIFace *IGadTools = 0;
#else
struct GfxBase *GfxBase = 0;
struct Library *GadToolsBase = 0;
#endif

#if defined(TG_GUI_HAVE_CYBERGRAPHICS)
struct Library *CyberGfxBase = 0;
static int tg_gui_cgx_open_attempted;
#if defined(__amigaos4__)
struct CyberGfxIFace *ICyberGfx = 0;
#endif

#if defined(TG_GUI_CGX_CLASSIC_RUNTIME)
/* CGraphX-DevKit FD/inline ABI, verified from the official developer archive:
   GetCyberMapAttr -0x60 (a0,d0), ReadPixelArray -0x78 and
   WritePixelArray -0x7e (a0,d0,d1,d2,a1,d3,d4,d5,d6,d7). */
static __inline ULONG tg_gui_cgx_get_map_attr(struct BitMap *bitmap,
                                               ULONG tag)
{
    register void *a6 __asm("a6") = CyberGfxBase;
    register struct BitMap *a0 __asm("a0") = bitmap;
    register ULONG d0 __asm("d0") = tag;

    /* d0/d1/a0/a1 are ABI scratch: the callee may trash them, so every one
       used as an argument must be in-out or the compiler will reuse a dead
       register after the call. */
    __asm __volatile("jsr a6@(-0x60)"
                     : "+r"(d0), "+r"(a0)
                     : "r"(a6)
                     : "d1", "a1", "cc", "memory");
    return d0;
}

static __inline ULONG tg_gui_cgx_read_pixel_array(
    APTR pixels, UWORD src_x, UWORD src_y, UWORD modulo,
    struct RastPort *rport, UWORD dst_x, UWORD dst_y,
    UWORD width, UWORD height, UBYTE format)
{
    register void *a6 __asm("a6") = CyberGfxBase;
    register APTR a0 __asm("a0") = pixels;
    register ULONG d0 __asm("d0") = src_x;
    register ULONG d1 __asm("d1") = src_y;
    register ULONG d2 __asm("d2") = modulo;
    register struct RastPort *a1 __asm("a1") = rport;
    register ULONG d3 __asm("d3") = dst_x;
    register ULONG d4 __asm("d4") = dst_y;
    register ULONG d5 __asm("d5") = width;
    register ULONG d6 __asm("d6") = height;
    register ULONG d7 __asm("d7") = format;

    __asm __volatile("jsr a6@(-0x78)"
                     : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                     : "r"(a6), "r"(d2),
                       "r"(d3), "r"(d4), "r"(d5), "r"(d6), "r"(d7)
                     : "cc", "memory");
    return d0;
}

static __inline ULONG tg_gui_cgx_write_pixel_array(
    APTR pixels, UWORD src_x, UWORD src_y, UWORD modulo,
    struct RastPort *rport, UWORD dst_x, UWORD dst_y,
    UWORD width, UWORD height, UBYTE format)
{
    register void *a6 __asm("a6") = CyberGfxBase;
    register APTR a0 __asm("a0") = pixels;
    register ULONG d0 __asm("d0") = src_x;
    register ULONG d1 __asm("d1") = src_y;
    register ULONG d2 __asm("d2") = modulo;
    register struct RastPort *a1 __asm("a1") = rport;
    register ULONG d3 __asm("d3") = dst_x;
    register ULONG d4 __asm("d4") = dst_y;
    register ULONG d5 __asm("d5") = width;
    register ULONG d6 __asm("d6") = height;
    register ULONG d7 __asm("d7") = format;

    __asm __volatile("jsr a6@(-0x7e)"
                     : "+r"(d0), "+r"(d1), "+r"(a0), "+r"(a1)
                     : "r"(a6), "r"(d2),
                       "r"(d3), "r"(d4), "r"(d5), "r"(d6), "r"(d7)
                     : "cc", "memory");
    return d0;
}
#else
#define tg_gui_cgx_get_map_attr GetCyberMapAttr
#define tg_gui_cgx_read_pixel_array ReadPixelArray
#define tg_gui_cgx_write_pixel_array WritePixelArray
#endif

static int tg_gui_amiga_open_cybergraphics(void)
{
    if (CyberGfxBase != 0) {
        return 1;
    }
    if (tg_gui_cgx_open_attempted) {
        return 0;
    }
    tg_gui_cgx_open_attempted = 1;
    CyberGfxBase = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 40);
#if defined(__amigaos4__)
    if (CyberGfxBase != 0) {
        ICyberGfx = (struct CyberGfxIFace *)GetInterface(
            CyberGfxBase, (CONST_STRPTR)"main", 1, 0);
        if (ICyberGfx == 0) {
            CloseLibrary(CyberGfxBase);
            CyberGfxBase = 0;
        }
    }
#endif
    return CyberGfxBase != 0;
}

static void tg_gui_amiga_close_cybergraphics(void)
{
#if defined(__amigaos4__)
    if (ICyberGfx != 0) {
        DropInterface((struct Interface *)ICyberGfx);
        ICyberGfx = 0;
    }
#endif
    if (CyberGfxBase != 0) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = 0;
    }
}
#else
static int tg_gui_amiga_open_cybergraphics(void)
{
    return 0;
}

static void tg_gui_amiga_close_cybergraphics(void)
{
}
#endif

/* Resolve the adaptive default only after Intuition has selected the actual
   screen. An explicit saved choice never enters this path. The result stays in
   memory for the whole run, including iconify/own-screen reopen cycles. */
static void tg_gui_window_resolve_inline_default(tg_gui_state *state,
                                                 struct Window *window)
{
#if defined(__amigaos3__)
    ULONG depth;
    int cpu_at_least_040;
    int has_rtg;

    if (state == 0 || window == 0 || state->inline_photos_explicit ||
        state->inline_photos_default_resolved) {
        return;
    }
    depth = GetBitMapAttr(window->WScreen->RastPort.BitMap, BMA_DEPTH);
    cpu_at_least_040 =
        SysBase != 0 &&
        (SysBase->AttnFlags & (AFF_68040 | AFF_68060)) != 0;
    has_rtg = depth > 8UL || tg_gui_amiga_open_cybergraphics();
    state->inline_photos = tg_gui_inline_photos_resolve(
        0, state->inline_photos, 1, cpu_at_least_040, has_rtg);
    state->inline_photos_default_resolved = 1;
    tg_gui_session_set_inline_photos(state->inline_photos);
    if (!state->inline_photos) {
        if (!has_rtg && !cpu_at_least_040) {
            tg_gui_log("photo: inline default off (no RTG / cpu < 040)");
        } else if (!has_rtg) {
            tg_gui_log("photo: inline default off (no RTG)");
        } else {
            tg_gui_log("photo: inline default off (cpu < 040)");
        }
    }
#else
    (void)state;
    (void)window;
#endif
}

/* Core GUI libraries share the window lifetime. Keep the required
   Intuition/Graphics pair and the optional GadTools menu in one symmetric
   owner so every OS4 interface is dropped before its base is closed. ASL and
   Workbench/Icon deliberately remain scoped to the requester and iconified
   wait: holding them here would increase the parked footprint. */
static void tg_gui_amiga_close_core_libs(void)
{
#if defined(__amigaos4__)
    if (IGadTools != 0) {
        DropInterface((struct Interface *)IGadTools);
        IGadTools = 0;
    }
#endif
    if (GadToolsBase != 0) {
        CloseLibrary(GadToolsBase);
        GadToolsBase = 0;
    }
#if defined(__amigaos4__)
    if (IGraphics != 0) {
        DropInterface((struct Interface *)IGraphics);
        IGraphics = 0;
    }
#endif
    if (GfxBase != 0) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = 0;
    }
#if defined(__amigaos4__)
    if (IIntuition != 0) {
        DropInterface((struct Interface *)IIntuition);
        IIntuition = 0;
    }
#endif
    if (IntuitionBase != 0) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
    }
}

static int tg_gui_amiga_open_core_libs(void)
{
#if defined(__amigaos4__)
    IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library", 39L);
#else
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39L);
#endif
    if (IntuitionBase == 0) {
        return 0;
    }
#if defined(__amigaos4__)
    IIntuition = (struct IntuitionIFace *)GetInterface(
        (struct Library *)IntuitionBase, "main", 1L, 0);
    if (IIntuition == 0) {
        tg_gui_amiga_close_core_libs();
        return 0;
    }
    GfxBase = OpenLibrary((CONST_STRPTR)"graphics.library", 39L);
    if (GfxBase != 0) {
        IGraphics = (struct GraphicsIFace *)GetInterface(GfxBase, "main", 1L,
                                                         0);
        if (IGraphics == 0) {
            CloseLibrary(GfxBase);
            GfxBase = 0;
        }
    }
#else
    GfxBase = (struct GfxBase *)OpenLibrary(
        (CONST_STRPTR)"graphics.library", 39L);
#endif
    if (GfxBase == 0) {
        tg_gui_amiga_close_core_libs();
        return 0;
    }

    /* A missing GadTools library only removes the menu, as before. */
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39L);
#if defined(__amigaos4__)
    if (GadToolsBase != 0) {
        IGadTools = (struct GadToolsIFace *)GetInterface(GadToolsBase, "main",
                                                         1L, 0);
        if (IGadTools == 0) {
            CloseLibrary(GadToolsBase);
            GadToolsBase = 0;
        }
    }
#endif
    return 1;
}

static void tg_gui_timer_arm(tg_gui_timereq *request, int fast)
{
    TG_GUI_TR_NODE(request).io_Command = TR_ADDREQUEST;
    TG_GUI_TR_SECS(request) = fast ? 0UL : TG_GUI_HEARTBEAT_SECS;
    TG_GUI_TR_MICRO(request) = fast ? TG_GUI_PHOTO_FAST_WAKE_US : 0UL;
    SendIO((struct IORequest *)request);
}

/* AfA_OS 4.8 replaces both graphics and diskfont internals. Their combined
   Text() path can freeze when a font is rendered into a layerless off-screen
   RastPort, although the same font strike and BltTemplate() remain sound.
   Inspect the loaded library list only: OpenLibrary() would create a false
   positive on systems where AfA is installed but not active. */
static int tg_gui_amiga_afa_text_compat(void)
{
#if defined(__amigaos3__)
    struct Node *node;

    Forbid();
    node = FindName(&SysBase->LibList,
                    (CONST_STRPTR)"afa_system.library");
    Permit();
    return node != 0;
#else
    return 0;
#endif
}

/* Menu item ids (GadTools NM_USERDATA), decoded on IDCMP_MENUPICK. */
#define TG_MENU_ABOUT  1
#define TG_MENU_HELP   2
#define TG_MENU_QUIT   3
#define TG_MENU_REMOVE 4
#define TG_MENU_SENDFILE 5
#define TG_MENU_ICONIFY 6
#define TG_MENU_OWNSCREEN 7
#define TG_MENU_COPY 8
#define TG_MENU_PASTE 9
#define TG_MENU_CUT 10
#define TG_MENU_RELOAD 11
#define TG_MENU_DLDIR 12
#define TG_MENU_SENDPHOTO 13
#define TG_MENU_INLINEPHOTOS 14
#define TG_MENU_DITHER_FULL 15
#define TG_MENU_DITHER_LIGHT 16
#define TG_MENU_DITHER_OFF 17
#define TG_MENU_CACHE_10 18
#define TG_MENU_CACHE_50 19
#define TG_MENU_CACHE_200 20
#define TG_MENU_CACHE_UNLIMITED 21
#define TG_MENU_CACHE_CLEAR 22
#define TG_MENU_EMOJI 23

/* Dark-theme palette: one RGB triplet per pen role and per avatar tint. The
   backend resolves the renderer's pen indices to obtained pens here; a future
   light / AmIRC theme is just another table. */
typedef struct tg_gui_rgb {
    unsigned char r;
    unsigned char g;
    unsigned char b;
} tg_gui_rgb;

static const tg_gui_rgb tg_gui_dark_pens[TG_GUI_PEN_COUNT] = {
    {0x12, 0x14, 0x1a}, /* WINDOW */
    {0x20, 0x24, 0x2e}, /* SURFACE */
    {0xe6, 0xea, 0xf0}, /* TEXT */
    {0x93, 0x9a, 0xa6}, /* TEXT_DIM */
    {0x2a, 0x6e, 0xb4}, /* ACCENT */
    {0xf0, 0xf6, 0xff}, /* ACCENT_TEXT */
    {0x1a, 0x2c, 0x44}, /* SELECT */
    {0x18, 0x5f, 0xa5}, /* BADGE */
    {0xe6, 0xf1, 0xfb}, /* BADGE_TEXT */
    {0x4d, 0xc2, 0xff}, /* READ - read-receipt double check, pops on the blue bubble */
    /* MENU_*: fallbacks only -- the real values come from the screen's
       DrawInfo in obtain_pens below (classic WB grey/black/blue here). */
    {0xaa, 0xaa, 0xaa}, /* MENU_BACK */
    {0x00, 0x00, 0x00}, /* MENU_TEXT */
    {0x00, 0x55, 0xaa}, /* MENU_FILL */
    {0xff, 0xff, 0xff}, /* MENU_FILLTEXT */
    {0x00, 0x00, 0x00}, /* MENU_FRAME */
    {0x6f, 0xb8, 0xff}  /* LINK - light blue, readable on the dark surface */
};

static const tg_gui_rgb tg_gui_avatar_rgb[TG_GUI_AVATAR_COLORS] = {
    {0x2f, 0x8f, 0x74}, /* teal */
    {0xc0, 0x5a, 0x3c}, /* coral */
    {0x6a, 0x5f, 0xc8}, /* purple */
    {0xbf, 0x52, 0x7e}, /* pink */
    {0xb0, 0x7a, 0x1f}, /* amber */
    {0x2c, 0x7c, 0xb8}  /* blue */
};

#define TG_GUI_PHOTO_DIRECT_OPS 24

struct tg_gui_photo_slot;

typedef struct tg_gui_photo_direct_op {
    struct tg_gui_photo_slot *slot;
    tg_gui_rect rect;
    int x0;
    int y0;
    int x1;
    int y1;
} tg_gui_photo_direct_op;

typedef struct tg_gui_amiga_ctx {
    struct Window *window;
    struct RastPort *rport;
    int origin_x;
    int origin_y;
    int inner_w;
    int inner_h;
    int line_h;
    LONG pens[TG_GUI_PEN_COUNT];               /* usable draw pen (fallback if obtain failed) */
    LONG pens_obtained[TG_GUI_PEN_COUNT];       /* raw ObtainBestPenA result, -1 if failed */
    LONG avatar_pens[TG_GUI_AVATAR_COLORS];
    LONG avatar_obtained[TG_GUI_AVATAR_COLORS];
    int pens_held;
    /* Off-screen double-buffer (flicker-free paint). buf_ok==0 => direct render. */
    struct BitMap *buf_bm;   /* friend of window->RPort->BitMap; 0 if none */
    struct RastPort buf_rp;  /* layerless RastPort over buf_bm */
    int buf_w;               /* allocated buffer width  (== inner_w when valid) */
    int buf_h;               /* allocated buffer height (== inner_h when valid) */
    int buf_ok;              /* 1 iff buf_bm and buf_rp.Font are valid */
    int bitmap_text_compat;  /* AfA_OS Text() cannot target this off-screen RP */
    int photo_truecolor;      /* optional cybergraphics RGB888 row replay */
    int photo_cgx_checked;    /* destination bitmap passed the RGB write/read test */
    int photo_cgx_usable;
    int photo_window_cgx_checked;
    int photo_window_cgx_usable;
    int photo_cgx_failed;     /* permanent pen fallback for this window session */
    int photo_viewer_scope;   /* fallback must not mutate transcript cache slots */
    int photo_resize_active;  /* no photo cache/replay work while buffer geometry moves */
    int photo_dither;         /* TG_GUI_PHOTO_DITHER_* for the pen-grid path */
    tg_gui_photo_direct_op photo_direct_ops[TG_GUI_PHOTO_DIRECT_OPS];
    int photo_direct_count;
    int photo_direct_logged;
    int photo_direct_report; /* 1 pass / -1 fallback; log only outside layer lock */
    unsigned int afa_profile_paints; /* bounded --gui-live-debug paint samples */
} tg_gui_amiga_ctx;

static int tg_gui_amiga_width(tg_gui_backend *backend)
{
    return ((tg_gui_amiga_ctx *)backend->context)->inner_w;
}

static int tg_gui_amiga_height(tg_gui_backend *backend)
{
    return ((tg_gui_amiga_ctx *)backend->context)->inner_h;
}

static int tg_gui_amiga_line_height(tg_gui_backend *backend)
{
    return ((tg_gui_amiga_ctx *)backend->context)->line_h;
}

/* Baseline to top of the glyph cell, straight from the RastPort's font, so the
   caret covers the letters instead of floating above them on systems whose
   default font is taller than topaz 8. */
static int tg_gui_amiga_font_ascent(tg_gui_backend *backend)
{
    const tg_gui_amiga_ctx *ctx = (const tg_gui_amiga_ctx *)backend->context;

    if (ctx == 0 || ctx->rport == 0 || ctx->rport->Font == 0) {
        return 0; /* renderer falls back to its own approximation */
    }
    return (int)ctx->rport->Font->tf_Baseline;
}

static unsigned long tg_gui_amiga_font_char_index(const struct TextFont *font,
                                                   unsigned int c)
{
    if (c < (unsigned int)font->tf_LoChar ||
        c > (unsigned int)font->tf_HiChar) {
        return (unsigned long)font->tf_HiChar -
               (unsigned long)font->tf_LoChar + 1UL;
    }
    return (unsigned long)c - (unsigned long)font->tf_LoChar;
}

/* Pixel advance used by tg_gui_amiga_blt_text(). AfA's TextLength() reports
   the native text engine's metrics, which can differ from the font-strike
   spacing used by our layerless-buffer fallback. Layout, click hit-testing and
   caret placement must use the exact same advance as the visible glyphs. */
static int tg_gui_amiga_bitmap_text_width(const tg_gui_amiga_ctx *ctx,
                                          const char *text,
                                          unsigned long length)
{
    const struct TextFont *font;
    unsigned long i;
    int width;
    int spacing;

    font = ctx->buf_rp.Font != 0 ? ctx->buf_rp.Font : ctx->rport->Font;
    if (font == 0) {
        return 0;
    }
    spacing = ctx->buf_rp.Font != 0 ? (int)ctx->buf_rp.TxSpacing
                                    : (int)ctx->rport->TxSpacing;
    width = 0;
    for (i = 0UL; i < length; ++i) {
        unsigned long index;

        index = tg_gui_amiga_font_char_index(
            font, (unsigned int)(unsigned char)text[i]);
        if (font->tf_CharSpace != 0) {
            width += (int)((WORD *)font->tf_CharSpace)[index];
        } else {
            width += (int)font->tf_XSize;
        }
        width += spacing;
    }
    return width;
}

/* Width of a plain run: TextLength, or the bitmap path on compat screens. */
static int tg_gui_amiga_run_width(tg_gui_amiga_ctx *ctx, const char *text,
                                  unsigned long length)
{
    if (length == 0UL) {
        return 0;
    }
    if (length > 0x7fffUL) {
        length = 0x7fffUL; /* TextLength count is 16-bit; clamp defensively */
    }
    if (ctx->bitmap_text_compat) {
        return tg_gui_amiga_bitmap_text_width(ctx, text, length);
    }
    return (int)TextLength(ctx->rport, (STRPTR)text, (UWORD)length);
}

/* The square an emoji occupies inline: the font height, so a glyph sits in
   the line like a wide letter. 16 px glyphs scale down to it (9 px on Topaz
   8, 13 on the PPC and AROS defaults), the way avatars scale into a row. */
static int tg_gui_amiga_emoji_cell(const tg_gui_amiga_ctx *ctx)
{
    int h = ctx->rport != 0 && ctx->rport->Font != 0
                ? (int)ctx->rport->Font->tf_YSize : 8;

    return h < 8 ? 8 : h;
}

/* Pens for the sheet palette, resolved once per session through the same
   colour matching the avatars use. Entry 0 is never drawn (transparent). */
static LONG tg_gui_av_pen_for(const unsigned char *rgb); /* defined with the avatars */
static unsigned char tg_gui_emoji_pen[256];
static int tg_gui_emoji_pen_ready;

static void tg_gui_amiga_emoji_pens(void)
{
    int k;

    if (tg_gui_emoji_pen_ready) {
        return;
    }
    for (k = 0; k < 255; ++k) {
        tg_gui_emoji_pen[k + 1] =
            (unsigned char)tg_gui_av_pen_for(tg_emoji_sheet_palette[k]);
    }
    tg_gui_emoji_pen_ready = 1;
}

/* Draws sheet glyph `index` into the size x size square at (x, y_top), rows
   of equal pens merged into one RectFill like the avatar painter, index 0
   skipped so the background shows through. */
static int tg_gui_amiga_glyph_image(tg_gui_backend *backend,
                                    unsigned long index, int x, int y_top,
                                    int size)
{
    tg_gui_amiga_ctx *ctx = (tg_gui_amiga_ctx *)backend->context;
    const unsigned char *px;
    int y;

    if (index >= tg_emoji_sheet_count || size <= 0 || ctx->rport == 0) {
        return 0;
    }
    tg_gui_amiga_emoji_pens();
    px = tg_emoji_sheet_pixels[index];
    SetDrMd(ctx->rport, JAM1);
    for (y = 0; y < size; ++y) {
        int sy = (y * TG_EMOJI_GLYPH_SIZE) / size;
        int xx = 0;

        while (xx < size) {
            int sx = (xx * TG_EMOJI_GLYPH_SIZE) / size;
            unsigned char v = px[sy * TG_EMOJI_GLYPH_SIZE + sx];
            int run = xx + 1;

            if (v == 0U) {
                ++xx;
                continue;
            }
            while (run < size &&
                   px[sy * TG_EMOJI_GLYPH_SIZE +
                      ((run * TG_EMOJI_GLYPH_SIZE) / size)] == v) {
                ++run;
            }
            SetAPen(ctx->rport, (LONG)tg_gui_emoji_pen[v]);
            RectFill(ctx->rport, ctx->origin_x + x + xx,
                     ctx->origin_y + y_top + y,
                     ctx->origin_x + x + run - 1,
                     ctx->origin_y + y_top + y);
            xx = run;
        }
    }
    return 1;
}

/* Under this cell size a 16 pixel picture reduced inline is a blob (Topaz 8
   gives 9), so the text emoticon stands in for it there; the picker keeps
   the pictures at their native size regardless. */
#define TG_GUI_EMOJI_INLINE_MIN 12

static int tg_gui_amiga_text_width(tg_gui_backend *backend, const char *text,
                                   unsigned long length)
{
    tg_gui_amiga_ctx *ctx = (tg_gui_amiga_ctx *)backend->context;
    unsigned long i = 0UL;
    unsigned long run_start = 0UL;
    unsigned long index;
    int w = 0;
    int cell = 0;

    while (i < length) {
        if (tg_gui_emoji_pair_at(text, length, i, &index)) {
            if (cell == 0) {
                cell = tg_gui_amiga_emoji_cell(ctx);
            }
            w += tg_gui_amiga_run_width(ctx, text + run_start, i - run_start);
            if (cell >= TG_GUI_EMOJI_INLINE_MIN) {
                w += cell;
            } else {
                const char *t = tg_gui_session_emoji_text(index);

                w += tg_gui_amiga_run_width(ctx, t, (unsigned long)strlen(t));
            }
            i += 2UL;
            run_start = i;
        } else {
            ++i;
        }
    }
    return w + tg_gui_amiga_run_width(ctx, text + run_start, i - run_start);
}

static LONG tg_gui_amiga_resolve_pen(tg_gui_amiga_ctx *ctx, int pen)
{
    if (pen >= TG_GUI_PEN_COUNT) {
        int avatar;

        avatar = pen - TG_GUI_PEN_COUNT;
        if (avatar < 0 || avatar >= TG_GUI_AVATAR_COLORS) {
            avatar = 0;
        }
        return ctx->avatar_pens[avatar];
    }
    if (pen < 0 || pen >= TG_GUI_PEN_COUNT) {
        pen = TG_GUI_PEN_TEXT;
    }
    return ctx->pens[pen];
}

/* Primitive-level trail (--gui-live-debug) for the FIRST off-screen paint
   only: one line BEFORE each backend call, so a crash inside a graphics
   primitive leaves its name and geometry as the log's last line (the AmiKit /
   PiStorm depth-24 hunt). Armed around the first buffered render -- never on
   the direct path, whose painter runs under LockLayerRom where this DOS I/O
   is forbidden. Text probes log position and LENGTH only, never content. */
static int tg_gui_prim_trail;
static int tg_gui_prim_n;
static int tg_gui_profile_active;
static unsigned long tg_gui_profile_prims;
static unsigned long tg_gui_profile_photo_rgb_rows;
static unsigned long tg_gui_profile_photo_pen_runs;
static unsigned long tg_gui_profile_afa_fallback_blits;

static void tg_gui_prim_log(const char *kind, int x, int y, int w, int h)
{
    char line[80];

    if (tg_gui_profile_active) {
        ++tg_gui_profile_prims;
    }
    if (!tg_gui_prim_trail) {
        return;
    }
    ++tg_gui_prim_n;
    sprintf(line, "prim %d: %s %d,%d %dx%d", tg_gui_prim_n, kind, x, y, w, h);
    tg_gui_log(line);
}

static void tg_gui_amiga_fill_rect(tg_gui_backend *backend, int pen,
                                   tg_gui_rect rect)
{
    tg_gui_amiga_ctx *ctx;
    int x0;
    int y0;
    int x1;
    int y1;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    x0 = ctx->origin_x + rect.x;
    y0 = ctx->origin_y + rect.y;
    x1 = x0 + rect.w - 1;
    y1 = y0 + rect.h - 1;
    tg_gui_prim_log("fill", x0, y0, rect.w, rect.h);
    SetAPen(ctx->rport, tg_gui_amiga_resolve_pen(ctx, pen));
    RectFill(ctx->rport, x0, y0, x1, y1);
}

/* Left inset of a disc of diameter h at pixel row y (doubled coordinates,
   tiny integer square root). Twin of the renderer's tg_gui_round_inset; the
   backend keeps its own copy because the renderer's is a different module's
   static. Avatars draw as discs with it, both the initials fallback and the
   decoded image, so the corners show the row background exactly like the
   desktop client's round avatars. */
static int tg_gui_amiga_disc_inset(int y, int h)
{
    int dy = (2 * y + 1) - h;
    int rr = (h * h) - (dy * dy);
    int dx = 0;

    if (rr <= 0) {
        return h / 2;
    }
    while ((dx + 1) * (dx + 1) <= rr) {
        ++dx;
    }
    return (h - dx) / 2;
}

/* Edge smoothing for the round chrome (0.0.91 field feedback: the circles
   looked stair-stepped). A one-pixel ring in the colour halfway between the
   shape and its background is exactly the anti-aliasing a hard-edged display
   can afford: the ring follows the true silhouette, the solid shape sits one
   pixel inside it. Blend pens are obtained lazily against the screen's
   colormap and cached; on paletted screens (no exact pens, tight budget) the
   cache stays empty and every shape keeps its hard edge, which is also what
   those screens' dithered look expects. */
#define TG_GUI_BLEND_PENS 24
static struct {
    ULONG rgb;
    LONG pen;
} tg_gui_blend_pens[TG_GUI_BLEND_PENS];
static int tg_gui_blend_count;
/* Defined with the avatar pen pool below; the blends share its colormap and
   its truecolor gate. */
static struct ColorMap *tg_gui_av_cmap;
static int tg_gui_av_rich;
static ULONG tg_gui_amiga_rgb32(unsigned char component);

static LONG tg_gui_amiga_blend_pen(const tg_gui_rgb *fg, const tg_gui_rgb *bg)
{
    ULONG key;
    unsigned char r;
    unsigned char g;
    unsigned char b;
    int i;
    LONG p;

    if (!tg_gui_av_rich || tg_gui_av_cmap == 0 || fg == 0 || bg == 0) {
        return -1;
    }
    r = (unsigned char)(((int)fg->r + (int)bg->r) / 2);
    g = (unsigned char)(((int)fg->g + (int)bg->g) / 2);
    b = (unsigned char)(((int)fg->b + (int)bg->b) / 2);
    key = ((ULONG)r << 16) | ((ULONG)g << 8) | (ULONG)b;
    for (i = 0; i < tg_gui_blend_count; ++i) {
        if (tg_gui_blend_pens[i].rgb == key) {
            return tg_gui_blend_pens[i].pen;
        }
    }
    if (tg_gui_blend_count >= TG_GUI_BLEND_PENS) {
        return -1;
    }
    p = ObtainBestPenA(tg_gui_av_cmap, tg_gui_amiga_rgb32(r),
                       tg_gui_amiga_rgb32(g), tg_gui_amiga_rgb32(b), 0);
    if (p == -1) {
        return -1;
    }
    tg_gui_blend_pens[tg_gui_blend_count].rgb = key;
    tg_gui_blend_pens[tg_gui_blend_count].pen = p;
    ++tg_gui_blend_count;
    return p;
}

static void tg_gui_amiga_blend_release(struct ColorMap *cmap)
{
    int i;

    for (i = 0; i < tg_gui_blend_count; ++i) {
        ReleasePen(cmap, tg_gui_blend_pens[i].pen);
    }
    tg_gui_blend_count = 0;
}

/* One RectFill per row, clipped to the pill silhouette (cap radius = h/2;
   with w == h it is a disc). Shared by the smoothed and the plain paths. */
static void tg_gui_amiga_pill_rows(tg_gui_amiga_ctx *ctx, LONG rawpen, int x,
                                   int y, int w, int h)
{
    int row;

    if (w <= 0 || h <= 0) {
        return;
    }
    SetAPen(ctx->rport, rawpen);
    /* Bands, not rows: consecutive rows share an inset (the body is one tall
       band), so a disc costs a handful of RectFills instead of one per row.
       On a 68k every primitive is real event-loop time, and a paint heavy
       enough to keep the port busy starves the photo pipeline. */
    row = 0;
    while (row < h) {
        int inset = tg_gui_amiga_disc_inset(row, h);
        int rw = w - (2 * inset);
        int last = row;

        while (last + 1 < h &&
               tg_gui_amiga_disc_inset(last + 1, h) == inset) {
            ++last;
        }
        if (rw > 0) {
            RectFill(ctx->rport, x + inset, y + row, x + inset + rw - 1,
                     y + last);
        }
        row = last + 1;
    }
}

/* Backend pill fill: the blend ring under a one-pixel-inset solid shape when
   the screen affords exact pens, plain hard rows otherwise. round_bg (set by
   the renderer before the call) names what the ring blends toward. */
static void tg_gui_amiga_fill_pill(tg_gui_backend *backend, int pen,
                                   tg_gui_rect rect)
{
    tg_gui_amiga_ctx *ctx;
    int x0;
    int y0;
    int bg;
    LONG blend;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    if (pen < 0 || pen >= TG_GUI_PEN_COUNT) {
        pen = 0;
    }
    if (rect.w < rect.h) { /* degenerate: the renderer's plain-box rule */
        tg_gui_amiga_fill_rect(backend, pen, rect);
        return;
    }
    x0 = ctx->origin_x + rect.x;
    y0 = ctx->origin_y + rect.y;
    tg_gui_prim_log("pill", x0, y0, rect.w, rect.h);
    bg = backend->round_bg;
    if (bg < 0 || bg >= TG_GUI_PEN_COUNT) {
        bg = TG_GUI_PEN_WINDOW;
    }
    blend = (rect.h >= 6)
                ? tg_gui_amiga_blend_pen(&tg_gui_dark_pens[pen],
                                         &tg_gui_dark_pens[bg])
                : -1;
    if (blend != -1) {
        tg_gui_amiga_pill_rows(ctx, blend, x0, y0, rect.w, rect.h);
        tg_gui_amiga_pill_rows(ctx, ctx->pens[pen], x0 + 1, y0 + 1,
                               rect.w - 2, rect.h - 2);
    } else {
        tg_gui_amiga_pill_rows(ctx, ctx->pens[pen], x0, y0, rect.w, rect.h);
    }
}

static void tg_gui_amiga_avatar_fill(tg_gui_backend *backend, int color_index,
                                     tg_gui_rect rect)
{
    tg_gui_amiga_ctx *ctx;
    int x0;
    int y0;
    int bg;
    LONG blend;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    if (color_index < 0 || color_index >= TG_GUI_AVATAR_COLORS) {
        color_index = 0;
    }
    x0 = ctx->origin_x + rect.x;
    y0 = ctx->origin_y + rect.y;
    tg_gui_prim_log("afill", x0, y0, rect.w, rect.h);
    bg = backend->round_bg;
    if (bg < 0 || bg >= TG_GUI_PEN_COUNT) {
        bg = TG_GUI_PEN_WINDOW;
    }
    /* Rows clipped to the disc: the corners keep whatever the row painted
       underneath. The blend ring smooths the silhouette on exact-pen
       screens; the initials still fit, the solid disc only loses one pixel
       of radius under the ring. */
    blend = (rect.h >= 6)
                ? tg_gui_amiga_blend_pen(&tg_gui_avatar_rgb[color_index],
                                         &tg_gui_dark_pens[bg])
                : -1;
    if (blend != -1) {
        tg_gui_amiga_pill_rows(ctx, blend, x0, y0, rect.w, rect.h);
        tg_gui_amiga_pill_rows(ctx, ctx->avatar_pens[color_index], x0 + 1,
                               y0 + 1, rect.w - 2, rect.h - 2);
    } else {
        tg_gui_amiga_pill_rows(ctx, ctx->avatar_pens[color_index], x0, y0,
                               rect.w, rect.h);
    }
}

/* --- real avatars (decoded stripped thumbs) ------------------------------
   Each avatar is decoded ONCE to a TG_GUI_AV_SZ^2 grid of RESOLVED PENS
   (1 byte/pixel) and cached by peer id; repaints replay the pen grid with
   run-length RectFills into the current (buffered) RastPort. Pens come from
   a small pool shared across ALL avatars (ObtainBestPenA, nearest-match once
   full, released with the other pens at teardown). Any failure returns 0 and
   the renderer falls back to the classic initials square. */
static ULONG tg_gui_amiga_rgb32(unsigned char component);

#define TG_GUI_AV_SZ 32
/* Must comfortably exceed the visible sidebar rows: negative slots live here
   too, and an eviction churn would bring back the per-repaint disk probing
   this cache exists to kill. ~1KB per slot. */
#if defined(__m68k__)
#define TG_GUI_AV_SLOTS 24
#else
#define TG_GUI_AV_SLOTS 32
#endif
/* Pen budget for the shared avatar pool, chosen at RUNTIME from the screen
   depth when the pool is armed at window open: paletted screens (<= 8 bit)
   get the lean profile (pens are scarce and shared with Workbench), truecolor
   RTG screens the rich one. This used to be a compile-time m68k gate, which
   kept avatars needlessly dull on truecolor RTG under OS3 (e.g. a Vampire):
   the machine is m68k but its screen affords the fine profile. Arrays are
   sized for the rich cap; the lean profile just uses less of them. */
#define TG_GUI_AV_POOL_MAX 160

typedef struct tg_gui_av_slot {
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long gen; /* store generation the slot was built at (retry gate) */
    int state; /* 0 free, 1 pens ready, -1 nothing/undecodable (initials) */
    unsigned char pen[TG_GUI_AV_SZ * TG_GUI_AV_SZ];
} tg_gui_av_slot;
static tg_gui_av_slot tg_gui_av_slots[TG_GUI_AV_SLOTS];
static unsigned long tg_gui_av_evict = 0UL;
static struct ColorMap *tg_gui_av_cmap = 0;
static LONG tg_gui_av_pool_pen[TG_GUI_AV_POOL_MAX];
static unsigned char tg_gui_av_pool_rgb[TG_GUI_AV_POOL_MAX][3];
static int tg_gui_av_pool_n = 0;
/* Runtime profile (set where the cmap is armed; lean defaults are the safe
   fallback if the depth probe ever fails). */
static int tg_gui_av_pool_cap = 48;   /* 48 paletted / 160 truecolor */
static long tg_gui_av_share_d = 192L; /* 192 paletted / 48 truecolor */
static int tg_gui_av_rich = 0;        /* seed: cube+greys vs greys only */

/* Message photos share the avatar pen pool but keep only a few CANONICAL pen
   grids. A slot is keyed only by Telegram photo id: bubble geometry never
   invalidates it, and every later paint is disk/decode/remap-free. */
#if defined(__m68k__)
#define TG_GUI_PHOTO_SLOTS 4
#define TG_GUI_PHOTO_JPEG_MAX (192UL * 1024UL)
#define TG_GUI_PHOTO_CANONICAL_CAP 256
#define TG_GUI_PHOTO_DECODE_CAP 512
#define TG_GUI_PHOTO_WORK_MIN 4UL
#define TG_GUI_PHOTO_WORK_INITIAL 12UL
#define TG_GUI_PHOTO_WORK_MAX 256UL
#define TG_GUI_PHOTO_MAX_DEFER_TICKS 6
/* A saturated event loop must not starve the photo pipeline for good: after
   MAX_DEFER_TICKS deferred turns the background turn is forced even with
   window events queued. Without this a paint heavy enough to keep a tick
   always pending stopped inline photos dead on 68k (Vampire field log:
   requests offered, nothing ever fetched or decoded). */
#define TG_GUI_PHOTO_FORCE_QUEUED_EVENTS 1
#define TG_GUI_PHOTO_VIEWER_JPEG_MAX (768UL * 1024UL)
#define TG_GUI_PHOTO_VIEWER_CANONICAL_CAP 512
#define TG_GUI_PHOTO_VIEWER_DECODE_CAP 768
#define TG_GUI_PHOTO_PREVIEW_CAP 128
#define TG_GUI_PHOTO_VIEWER_PREVIEW_CAP 160
#define TG_GUI_PHOTO_CACHE_MIN (16UL * 1024UL)
#define TG_GUI_PHOTO_CACHE_INITIAL (16UL * 1024UL)
#define TG_GUI_PHOTO_CACHE_MAX (1024UL * 1024UL)
#else
#define TG_GUI_PHOTO_SLOTS 6
#define TG_GUI_PHOTO_JPEG_MAX (1024UL * 1024UL)
#define TG_GUI_PHOTO_CANONICAL_CAP 448
#define TG_GUI_PHOTO_DECODE_CAP 768
#define TG_GUI_PHOTO_WORK_MIN 64UL
#define TG_GUI_PHOTO_WORK_INITIAL 192UL
#define TG_GUI_PHOTO_WORK_MAX 1024UL
#define TG_GUI_PHOTO_MAX_DEFER_TICKS 1
#define TG_GUI_PHOTO_FORCE_QUEUED_EVENTS 1
#define TG_GUI_PHOTO_VIEWER_JPEG_MAX (2UL * 1024UL * 1024UL)
#define TG_GUI_PHOTO_VIEWER_CANONICAL_CAP 768
#define TG_GUI_PHOTO_VIEWER_DECODE_CAP 1024
#define TG_GUI_PHOTO_PREVIEW_CAP 192
#define TG_GUI_PHOTO_VIEWER_PREVIEW_CAP 256
#define TG_GUI_PHOTO_CACHE_MIN (384UL * 1024UL)
#define TG_GUI_PHOTO_CACHE_INITIAL (384UL * 1024UL)
#define TG_GUI_PHOTO_CACHE_MAX (3UL * 1024UL * 1024UL)
#endif
#define TG_GUI_PHOTO_REPLAY_CAP TG_GUI_PHOTO_VIEWER_CANONICAL_CAP
#define TG_GUI_PHOTO_REQUESTS 24
#define TG_GUI_PHOTO_VISIBLE_MAX 24
#define TG_GUI_PHOTO_PACE_TARGET_MS 120UL
#define TG_GUI_PHOTO_LOCAL_STEPS_MAX 32
#define TG_GUI_PHOTO_CACHE_SCAN_PER_TICK 16
#define TG_GUI_PHOTO_CACHE_DELETE_PER_TICK 8
#define TG_GUI_PHOTO_CACHE_NAME_MAX 108

#define TG_GUI_PHOTO_WORK_NONE 0
#define TG_GUI_PHOTO_WORK_DECODE 1
#define TG_GUI_PHOTO_WORK_CACHE 2
#define TG_GUI_PHOTO_WORK_REPLAY 3

#define TG_GUI_PHOTO_SCOPE_INLINE 0
#define TG_GUI_PHOTO_SCOPE_VIEWER 1

#define TG_GUI_PHOTO_STALL_NONE 0
#define TG_GUI_PHOTO_STALL_INTERACTIVE 1
#define TG_GUI_PHOTO_STALL_RESIZE 2
#define TG_GUI_PHOTO_STALL_TRANSFER 3
#define TG_GUI_PHOTO_STALL_QUEUED_EVENT 4
#define TG_GUI_PHOTO_STALL_RESUME 5

typedef struct tg_gui_photo_slot {
    unsigned long id_hi;
    unsigned long id_lo;
    int w;
    int h;
    int state; /* 0 free, 1 complete, 2 decode/map in progress, -1 rejected */
    int ready_rows;       /* displayed RGB rows: either 0 or h */
    int pen_rows;         /* displayed pen rows: either 0 or h */
    int stage_ready_rows; /* current hidden quality pass */
    int stage_pen_rows;
    int decode_done;      /* current hidden pass entropy is complete */
    int quality_done;     /* final pass was committed */
    int preview_only;     /* stripped-thumb frame; replace when JPEG arrives */
    int pass_scale;
    int final_scale;
    int decode_w;         /* hidden quality-pass geometry */
    int decode_h;
    int render_logged;
    unsigned long last_use;
    FILE *canonical_file;
    unsigned long canonical_size;
    unsigned long canonical_loaded;
    int canonical_large;
    int canonical_from_disk;
    unsigned char *jpeg;
    unsigned long jpeg_len;
    tg_image_jpeg_decoder *decoder;
    unsigned char *pen;
    unsigned char *rgb;
    unsigned char *stage_pen;
    unsigned char *stage_rgb;
} tg_gui_photo_slot;

typedef struct tg_gui_photo_request {
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned long source_w;
    unsigned long source_h;
} tg_gui_photo_request;

typedef struct tg_gui_photo_visible {
    unsigned long id_hi;
    unsigned long id_lo;
} tg_gui_photo_visible;

/* The popup owns one dedicated canonical image. It never participates in the
   transcript LRU, so opening a large photo cannot evict an inline one. */
typedef struct tg_gui_photo_viewer {
    tg_gui_amiga_ctx ctx;
    tg_gui_photo_slot slot;
    unsigned long source_w;
    unsigned long source_h;
    char title[TG_GUI_NAME_MAX];
} tg_gui_photo_viewer;

typedef struct tg_gui_photo_save_job {
    int pending;
    int last_percent;
    unsigned long id_hi;
    unsigned long id_lo;
    char destination[256];
} tg_gui_photo_save_job;

static tg_gui_photo_slot tg_gui_photo_slots[TG_GUI_PHOTO_SLOTS];
static unsigned long tg_gui_photo_use_clock;
static tg_gui_photo_request tg_gui_photo_requests[TG_GUI_PHOTO_REQUESTS];
static int tg_gui_photo_request_count;
static tg_gui_photo_visible tg_gui_photo_visible_ids[TG_GUI_PHOTO_VISIBLE_MAX];
static int tg_gui_photo_visible_count;
static tg_gui_photo_decode_gate tg_gui_photo_decode_pipeline;

static void tg_gui_window_copy(char *dest, unsigned long size,
                               const char *src);

static void tg_gui_photo_diag(const char *message)
{
    if (tg_gui_log_is_enabled()) {
        tg_gui_log(message);
    }
}

/* DateStamp is available on every native lane and has 20 ms granularity on
   standard systems. Unsigned subtraction keeps short intervals correct across
   midnight and the 32-bit millisecond wrap. */
static unsigned long tg_gui_photo_now_ms(void)
{
    struct DateStamp stamp;
    unsigned long minutes;

    DateStamp(&stamp);
    minutes = (unsigned long)stamp.ds_Days * 24UL * 60UL +
              (unsigned long)stamp.ds_Minute;
    return minutes * 60UL * 1000UL +
           ((unsigned long)stamp.ds_Tick * 1000UL) /
               (unsigned long)TICKS_PER_SECOND;
}

static unsigned long tg_gui_photo_elapsed_ms(unsigned long start,
                                             unsigned long end)
{
    return end - start;
}

static void tg_gui_photo_pace_observe_log(const char *phase,
                                          tg_gui_photo_pace *pace,
                                          unsigned long elapsed_ms)
{
    if (tg_gui_photo_pace_observe(pace, elapsed_ms) &&
        tg_gui_log_is_enabled()) {
        char line[96];

        sprintf(line, "photo: pace %s budget %lu (slice %lums)",
                phase != 0 ? phase : "unknown", pace->budget, elapsed_ms);
        tg_gui_log(line);
    }
}

static int tg_gui_photo_pipeline_acquire(tg_gui_photo_slot *slot, int scope)
{
    return slot != 0 && tg_gui_photo_decode_gate_acquire(
        &tg_gui_photo_decode_pipeline, slot, slot->id_hi, slot->id_lo, scope);
}

static int tg_gui_photo_pipeline_owns(const tg_gui_photo_slot *slot, int scope)
{
    return slot != 0 && tg_gui_photo_decode_gate_owns(
        &tg_gui_photo_decode_pipeline, slot, slot->id_hi, slot->id_lo, scope);
}

static tg_gui_photo_slot *tg_gui_photo_pipeline_owner(int scope)
{
    tg_gui_photo_slot *slot;

    if (tg_gui_photo_decode_pipeline.owner == 0 ||
        tg_gui_photo_decode_pipeline.scope != scope) {
        return 0;
    }
    slot = (tg_gui_photo_slot *)tg_gui_photo_decode_pipeline.owner;
    if (!tg_gui_photo_pipeline_owns(slot, scope)) {
        tg_gui_photo_diag("photo: decode owner mismatch");
        tg_gui_photo_decode_gate_reset(&tg_gui_photo_decode_pipeline);
        return 0;
    }
    return slot;
}

static void tg_gui_photo_pipeline_release(tg_gui_photo_slot *slot)
{
    tg_gui_photo_decode_gate_release(&tg_gui_photo_decode_pipeline, slot);
}

static void tg_gui_photo_stall_diag(int reason)
{
    if (reason == TG_GUI_PHOTO_STALL_INTERACTIVE) {
        tg_gui_photo_diag("photo: queue stalled (interactive events)");
    } else if (reason == TG_GUI_PHOTO_STALL_RESIZE) {
        tg_gui_photo_diag("photo: queue stalled (resize)");
    } else if (reason == TG_GUI_PHOTO_STALL_TRANSFER) {
        tg_gui_photo_diag("photo: queue stalled (manual transfer)");
    } else if (reason == TG_GUI_PHOTO_STALL_QUEUED_EVENT) {
        tg_gui_photo_diag("photo: queue stalled (queued window event)");
    } else if (reason == TG_GUI_PHOTO_STALL_RESUME) {
        tg_gui_photo_diag("photo: queue stalled (resize resume)");
    }
}

static void tg_gui_photo_slot_clear(tg_gui_photo_slot *slot)
{
    if (slot == 0) {
        return;
    }
    tg_gui_photo_pipeline_release(slot);
    if (slot->canonical_file != 0) {
        fclose(slot->canonical_file);
    }
    tg_image_jpeg_decoder_destroy(slot->decoder);
    free(slot->jpeg);
    free(slot->pen);
    free(slot->rgb);
    free(slot->stage_pen);
    free(slot->stage_rgb);
    memset(slot, 0, sizeof(*slot));
}

static void tg_gui_photo_slots_reset(void)
{
    int i;

    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        tg_gui_photo_slot_clear(&tg_gui_photo_slots[i]);
    }
    tg_gui_photo_use_clock = 0UL;
    tg_gui_photo_request_count = 0;
    tg_gui_photo_visible_count = 0;
    tg_gui_photo_decode_gate_reset(&tg_gui_photo_decode_pipeline);
}

static void tg_gui_av_reset(void)
{
    int i;

    for (i = 0; i < TG_GUI_AV_SLOTS; ++i) {
        tg_gui_av_slots[i].state = 0;
    }
    tg_gui_av_pool_n = 0;
    tg_gui_av_evict = 0UL;
    tg_gui_photo_slots_reset();
}

static void tg_gui_av_release_pool(struct ColorMap *cmap)
{
    int i;

    for (i = 0; i < tg_gui_av_pool_n; ++i) {
        ReleasePen(cmap, tg_gui_av_pool_pen[i]);
    }
    tg_gui_av_pool_n = 0;
    tg_gui_av_cmap = 0;
    tg_gui_av_reset();
}

/* Drops the cached pen grid for one peer so the next paint rebuilds it from
   the freshest source (called after a successful avatar download). */
void tg_gui_window_avatar_invalidate(unsigned long id_hi, unsigned long id_lo)
{
    int i;

    for (i = 0; i < TG_GUI_AV_SLOTS; ++i) {
        if (tg_gui_av_slots[i].state != 0 &&
            tg_gui_av_slots[i].id_hi == id_hi &&
            tg_gui_av_slots[i].id_lo == id_lo) {
            tg_gui_av_slots[i].state = 0;
        }
    }
}

/* Obtain one pool pen for an exact RGB (used by the seeder; the miss path in
   pen_for keeps its own inline copy because it needs the pen value back).
   Skips silently when the pool is full or the obtain fails. */
static void tg_gui_av_pool_add(unsigned char r, unsigned char g,
                               unsigned char b)
{
    LONG p;

    if (tg_gui_av_pool_n >= tg_gui_av_pool_cap || tg_gui_av_cmap == 0) {
        return;
    }
    p = ObtainBestPenA(tg_gui_av_cmap, tg_gui_amiga_rgb32(r),
                       tg_gui_amiga_rgb32(g), tg_gui_amiga_rgb32(b), 0);
    if (p != -1) {
        tg_gui_av_pool_pen[tg_gui_av_pool_n] = p;
        tg_gui_av_pool_rgb[tg_gui_av_pool_n][0] = r;
        tg_gui_av_pool_rgb[tg_gui_av_pool_n][1] = g;
        tg_gui_av_pool_rgb[tg_gui_av_pool_n][2] = b;
        ++tg_gui_av_pool_n;
    }
}

/* Pre-seed the shared pool with a neutral colour lattice. Without this the
   first 2-3 avatars filled the pool with THEIR shades and, once full, every
   later colour snapped to the nearest of those with no bound: whites picked
   up a pink or blue cast depending on which avatars happened to paint first
   (tester reports: pinkish whites on MorphOS, blueish on OS4 -- same code,
   different chat lists). A fixed 4x4x4 RGB cube plus a grey ramp bounds the
   full-pool fallback error and keeps neutrals neutral; the remaining slots
   stay dynamic for frequent exact colours. Paletted screens (lean profile,
   any CPU) seed greys only: pens are scarce there and the coarse share step
   already merges shades. */
static void tg_gui_av_seed_pool(void)
{
    static const unsigned char lv[4] = { 0U, 85U, 170U, 255U };
    int r;
    int g;
    int b;
    int i;

    if (tg_gui_av_rich) { /* truecolor: cube + greys, then dynamic colours */
        for (r = 0; r < 4; ++r) {
            for (g = 0; g < 4; ++g) {
                for (b = 0; b < 4; ++b) {
                    tg_gui_av_pool_add(lv[r], lv[g], lv[b]);
                }
            }
        }
        for (i = 17; i < 255; i += 17) {
            if ((i % 85) != 0) { /* skip the greys already in the cube */
                tg_gui_av_pool_add((unsigned char)i, (unsigned char)i,
                                   (unsigned char)i);
            }
        }
    } else { /* paletted: 6 greys keep whites neutral on a lean budget */
        for (i = 0; i <= 255; i += 51) {
            tg_gui_av_pool_add((unsigned char)i, (unsigned char)i,
                               (unsigned char)i);
        }
    }
}

/* Pool pen for an RGB pixel: reuse a close pool entry, else obtain a new one
   (PRECISION-default like the theme pens), else nearest of what we have. */
static LONG tg_gui_av_pen_for(const unsigned char *rgb)
{
    int i;
    int best = -1;
    long best_d = 0x7fffffffL;

    if (tg_gui_av_pool_n == 0) {
        tg_gui_av_seed_pool(); /* once per screen (reset drops the pool) */
    }
    for (i = 0; i < tg_gui_av_pool_n; ++i) {
        long dr = (long)tg_gui_av_pool_rgb[i][0] - (long)rgb[0];
        long dg = (long)tg_gui_av_pool_rgb[i][1] - (long)rgb[1];
        long db = (long)tg_gui_av_pool_rgb[i][2] - (long)rgb[2];
        long d = dr * dr + dg * dg + db * db;

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best >= 0 && best_d <= tg_gui_av_share_d) {
        return tg_gui_av_pool_pen[best]; /* close enough: share */
    }
    if (tg_gui_av_pool_n < tg_gui_av_pool_cap && tg_gui_av_cmap != 0) {
        LONG p = ObtainBestPenA(tg_gui_av_cmap, tg_gui_amiga_rgb32(rgb[0]),
                                tg_gui_amiga_rgb32(rgb[1]),
                                tg_gui_amiga_rgb32(rgb[2]), 0);

        if (p != -1) {
            tg_gui_av_pool_pen[tg_gui_av_pool_n] = p;
            tg_gui_av_pool_rgb[tg_gui_av_pool_n][0] = rgb[0];
            tg_gui_av_pool_rgb[tg_gui_av_pool_n][1] = rgb[1];
            tg_gui_av_pool_rgb[tg_gui_av_pool_n][2] = rgb[2];
            ++tg_gui_av_pool_n;
            return p;
        }
    }
    return (best >= 0) ? tg_gui_av_pool_pen[best] : -1;
}

/* Configurable Bayer 4x4 ordered dither for the pen-grid fallback. Truecolor
   replay never calls this path. It runs while a canonical slot is built, not
   during repaint, so changing the preference deliberately rebuilds the slot. */
static LONG tg_gui_photo_pen_for(const tg_gui_amiga_ctx *ctx,
                                 const unsigned char *rgb, int x, int y)
{
    unsigned char adjusted[3];
    int amplitude;

    amplitude = 4;
    if (ctx != 0 && ctx->photo_dither == TG_GUI_PHOTO_DITHER_LIGHT) {
        amplitude = 2;
    } else if (ctx != 0 &&
               ctx->photo_dither == TG_GUI_PHOTO_DITHER_OFF) {
        amplitude = 0;
    }
    tg_image_ordered_dither_rgb_level(rgb, x, y, amplitude, adjusted);
    return tg_gui_av_pen_for(adjusted);
}

static int tg_gui_amiga_avatar_image(tg_gui_backend *backend,
                                     unsigned long id_hi, unsigned long id_lo,
                                     tg_gui_rect rect)
{
    tg_gui_amiga_ctx *ctx;
    tg_gui_av_slot *slot;
    int i;
    int y;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    if (rect.w <= 0 || rect.h <= 0 || (id_hi == 0UL && id_lo == 0UL) ||
        tg_gui_av_cmap == 0) {
        return 0;
    }
    tg_gui_prim_log("aimg", rect.x, rect.y, rect.w, rect.h);
    slot = 0;
    for (i = 0; i < TG_GUI_AV_SLOTS; ++i) {
        if (tg_gui_av_slots[i].state != 0 &&
            tg_gui_av_slots[i].id_hi == id_hi &&
            tg_gui_av_slots[i].id_lo == id_lo) {
            slot = &tg_gui_av_slots[i];
            /* A negative slot only retries when NEW thumbs arrived since it
               was cached; otherwise repaints must stay I/O-free. */
            if (slot->state == -1 &&
                slot->gen != tg_mtproto_avatar_store_generation()) {
                slot->state = 0;
                slot = 0;
            }
            break;
        }
    }
    if (slot == 0) {
        const unsigned char *thumb;
        unsigned long thumb_len;
        static unsigned char rgb[TG_GUI_AV_SZ * TG_GUI_AV_SZ * 3];
        unsigned long px;
        int have_rgb = 0;

        /* Source priority: the downloaded 160px JPEG on disk (v2, crisp),
           else the inline stripped thumb (v1, blurred), else initials. */
        {
            char name[48];
            FILE *f;

            sprintf(name, "avatars/tgav%08lx%08lx.jpg", id_hi, id_lo);
            f = fopen(name, "rb");
            if (f != 0) {
                static unsigned char jpeg[24576];
                unsigned long n = (unsigned long)fread(jpeg, 1, sizeof(jpeg),
                                                       f);

                fclose(f);
                if (n > 0UL && n < sizeof(jpeg) &&
                    tg_avatar_decode_jpeg(jpeg, n, rgb, TG_GUI_AV_SZ,
                                          TG_GUI_AV_SZ) == 0) {
                    have_rgb = 1;
                }
            }
        }
        if (!have_rgb) {
            if (!tg_mtproto_avatar_thumb_lookup(id_hi, id_lo, &thumb,
                                                &thumb_len)) {
                thumb = 0; /* nothing yet: cache the miss as a negative slot
                              below, so the next repaint skips the disk probe */
                thumb_len = 0UL;
            }
        }
        for (i = 0; i < TG_GUI_AV_SLOTS; ++i) {
            if (tg_gui_av_slots[i].state == 0) {
                slot = &tg_gui_av_slots[i];
                break;
            }
        }
        if (slot == 0) {
            slot = &tg_gui_av_slots[tg_gui_av_evict % TG_GUI_AV_SLOTS];
            ++tg_gui_av_evict;
        }
        slot->id_hi = id_hi;
        slot->id_lo = id_lo;
        slot->gen = tg_mtproto_avatar_store_generation();
        if (!have_rgb &&
            (thumb == 0 ||
             tg_avatar_decode_stripped(thumb, thumb_len, rgb, TG_GUI_AV_SZ,
                                       TG_GUI_AV_SZ) != 0)) {
            slot->state = -1; /* nothing/undecodable: initials, no re-probe */
            return 0;
        }
        for (px = 0UL; px < TG_GUI_AV_SZ * TG_GUI_AV_SZ; ++px) {
            LONG p = tg_gui_av_pen_for(rgb + px * 3UL);

            if (p == -1) { /* pen system exhausted: give up cleanly */
                slot->state = -1;
                return 0;
            }
            slot->pen[px] = (unsigned char)p;
        }
        slot->state = 1;
    }
    if (slot->state != 1) {
        return 0;
    }
    /* Replay the pen grid scaled to rect (nearest), row by row with
       run-length RectFills into the current (buffered) RastPort, each row
       clipped to the disc so the avatar comes out round. */
    for (y = 0; y < rect.h; ++y) {
        int sy = (y * TG_GUI_AV_SZ) / rect.h;
        int row_inset = tg_gui_amiga_disc_inset(y, rect.h);
        int row_end = rect.w - row_inset;
        int x = row_inset;

        if (row_end <= x) {
            continue;
        }
        while (x < row_end) {
            int sx = (x * TG_GUI_AV_SZ) / rect.w;
            unsigned char p = slot->pen[sy * TG_GUI_AV_SZ + sx];
            int run = x + 1;

            while (run < row_end &&
                   slot->pen[sy * TG_GUI_AV_SZ +
                             ((run * TG_GUI_AV_SZ) / rect.w)] == p) {
                ++run;
            }
            SetAPen(ctx->rport, (LONG)p);
            RectFill(ctx->rport, ctx->origin_x + rect.x + x,
                     ctx->origin_y + rect.y + y,
                     ctx->origin_x + rect.x + run - 1,
                     ctx->origin_y + rect.y + y);
            x = run;
        }
    }
    return 1;
}

/* A full paint rebuilds visibility in transcript order (top to bottom). It also
   rebuilds the pending queue: active partial decoders keep their slots, while
   every still-missing visible photo is offered again by photo_image(). */
static void tg_gui_photo_frame_begin(void)
{
    tg_gui_photo_visible_count = 0;
    tg_gui_photo_request_count = 0;
}

static int tg_gui_photo_visible_priority(unsigned long id_hi,
                                         unsigned long id_lo)
{
    int i;

    for (i = 0; i < tg_gui_photo_visible_count; ++i) {
        if (tg_gui_photo_visible_ids[i].id_hi == id_hi &&
            tg_gui_photo_visible_ids[i].id_lo == id_lo) {
            return i;
        }
    }
    return -1;
}

static int tg_gui_photo_visible_mark(unsigned long id_hi,
                                     unsigned long id_lo)
{
    int priority;

    priority = tg_gui_photo_visible_priority(id_hi, id_lo);
    if (priority >= 0) {
        return priority;
    }
    if (tg_gui_photo_visible_count >= TG_GUI_PHOTO_VISIBLE_MAX) {
        return TG_GUI_PHOTO_VISIBLE_MAX;
    }
    priority = tg_gui_photo_visible_count;
    tg_gui_photo_visible_ids[priority].id_hi = id_hi;
    tg_gui_photo_visible_ids[priority].id_lo = id_lo;
    ++tg_gui_photo_visible_count;
    return priority;
}

/* The disk cache is catalogued once, incrementally, after the GUI starts. The
   policy array is portable and host-tested; filenames/Telegram ids live in a
   parallel native array so pruning can use the exact same planner without a
   second full-sized copy on memory-constrained machines. */
typedef struct tg_gui_photo_cache_meta {
    char name[TG_GUI_PHOTO_CACHE_NAME_MAX];
    unsigned long id_hi;
    unsigned long id_lo;
    unsigned char delete_failures;
} tg_gui_photo_cache_meta;

typedef struct tg_gui_photo_cache_manager {
    BPTR directory;
    struct FileInfoBlock fib;
    tg_gui_photo_cache_item *items;
    tg_gui_photo_cache_meta *meta;
    int count;
    int capacity;
    int active;
    int scan_complete;
    int scan_failed;
    int prune_pending;
    int clear_pending;
    unsigned long long limit_bytes;
    unsigned long long total_bytes;
    unsigned long clear_files;
    unsigned long long clear_bytes;
} tg_gui_photo_cache_manager;

static tg_gui_photo_cache_manager tg_gui_photo_disk_cache;

static int tg_gui_photo_cache_hex8(const char *text, unsigned long *value)
{
    unsigned long out;
    int i;

    if (text == 0 || value == 0) {
        return 0;
    }
    out = 0UL;
    for (i = 0; i < 8; ++i) {
        unsigned long digit;
        char c;

        c = text[i];
        if (c >= '0' && c <= '9') {
            digit = (unsigned long)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned long)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned long)(c - 'A' + 10);
        } else {
            return 0;
        }
        out = (out << 4) | digit;
    }
    *value = out;
    return 1;
}

static void tg_gui_photo_cache_parse_id(const char *name,
                                        unsigned long *id_hi,
                                        unsigned long *id_lo)
{
    unsigned long hi;
    unsigned long lo;

    if (id_hi != 0) {
        *id_hi = 0UL;
    }
    if (id_lo != 0) {
        *id_lo = 0UL;
    }
    if (name == 0 || strlen(name) < 20UL ||
        strncmp(name, "tgph", 4UL) != 0 ||
        !tg_gui_photo_cache_hex8(name + 4, &hi) ||
        !tg_gui_photo_cache_hex8(name + 12, &lo)) {
        return;
    }
    if (id_hi != 0) {
        *id_hi = hi;
    }
    if (id_lo != 0) {
        *id_lo = lo;
    }
}

static const char *tg_gui_photo_cache_basename(const char *path)
{
    const char *base;
    const char *p;

    base = path;
    if (path == 0) {
        return "";
    }
    for (p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == ':') {
            base = p + 1;
        }
    }
    return base;
}

static int tg_gui_photo_cache_find(const char *name)
{
    int i;

    for (i = 0; i < tg_gui_photo_disk_cache.count; ++i) {
        if (strcmp(tg_gui_photo_disk_cache.meta[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int tg_gui_photo_cache_reserve(int need)
{
    tg_gui_photo_cache_item *new_items;
    tg_gui_photo_cache_meta *new_meta;
    int capacity;

    if (need <= tg_gui_photo_disk_cache.capacity) {
        return 1;
    }
    capacity = tg_gui_photo_disk_cache.capacity > 0
                   ? tg_gui_photo_disk_cache.capacity * 2 : 32;
    while (capacity < need) {
        capacity *= 2;
    }
    new_items = (tg_gui_photo_cache_item *)malloc(
        (size_t)capacity * sizeof(*new_items));
    new_meta = (tg_gui_photo_cache_meta *)malloc(
        (size_t)capacity * sizeof(*new_meta));
    if (new_items == 0 || new_meta == 0) {
        free(new_items);
        free(new_meta);
        return 0;
    }
    if (tg_gui_photo_disk_cache.count > 0) {
        memcpy(new_items, tg_gui_photo_disk_cache.items,
               (size_t)tg_gui_photo_disk_cache.count * sizeof(*new_items));
        memcpy(new_meta, tg_gui_photo_disk_cache.meta,
               (size_t)tg_gui_photo_disk_cache.count * sizeof(*new_meta));
    }
    free(tg_gui_photo_disk_cache.items);
    free(tg_gui_photo_disk_cache.meta);
    tg_gui_photo_disk_cache.items = new_items;
    tg_gui_photo_disk_cache.meta = new_meta;
    tg_gui_photo_disk_cache.capacity = capacity;
    return 1;
}

static int tg_gui_photo_cache_upsert(const char *name, unsigned long bytes,
                                     const struct DateStamp *stamp)
{
    int at;
    tg_gui_photo_cache_item *item;
    tg_gui_photo_cache_meta *meta;

    if (name == 0 || name[0] == '\0' ||
        strlen(name) >= TG_GUI_PHOTO_CACHE_NAME_MAX) {
        return 0;
    }
    at = tg_gui_photo_cache_find(name);
    if (at < 0) {
        if (!tg_gui_photo_cache_reserve(tg_gui_photo_disk_cache.count + 1)) {
            return 0;
        }
        at = tg_gui_photo_disk_cache.count++;
        memset(&tg_gui_photo_disk_cache.items[at], 0,
               sizeof(tg_gui_photo_disk_cache.items[at]));
        memset(&tg_gui_photo_disk_cache.meta[at], 0,
               sizeof(tg_gui_photo_disk_cache.meta[at]));
        strcpy(tg_gui_photo_disk_cache.meta[at].name, name);
        tg_gui_photo_cache_parse_id(
            name, &tg_gui_photo_disk_cache.meta[at].id_hi,
            &tg_gui_photo_disk_cache.meta[at].id_lo);
    } else if (tg_gui_photo_disk_cache.items[at].bytes <=
               tg_gui_photo_disk_cache.total_bytes) {
        tg_gui_photo_disk_cache.total_bytes -=
            (unsigned long long)tg_gui_photo_disk_cache.items[at].bytes;
    } else {
        tg_gui_photo_disk_cache.total_bytes = 0ULL;
    }
    item = &tg_gui_photo_disk_cache.items[at];
    meta = &tg_gui_photo_disk_cache.meta[at];
    item->bytes = bytes;
    item->days = stamp != 0 ? (unsigned long)stamp->ds_Days : 0UL;
    item->minutes = stamp != 0 ? (unsigned long)stamp->ds_Minute : 0UL;
    item->ticks = stamp != 0 ? (unsigned long)stamp->ds_Tick : 0UL;
    item->protected_entry = 0;
    item->remove = 0;
    meta->delete_failures = 0;
    tg_gui_photo_disk_cache.total_bytes += (unsigned long long)bytes;
    if (tg_gui_photo_disk_cache.scan_complete &&
        tg_gui_photo_disk_cache.limit_bytes != 0ULL &&
        tg_gui_photo_disk_cache.total_bytes >
            tg_gui_photo_disk_cache.limit_bytes) {
        tg_gui_photo_disk_cache.prune_pending = 1;
    }
    return 1;
}

static void tg_gui_photo_cache_remove_at(int at)
{
    if (at < 0 || at >= tg_gui_photo_disk_cache.count) {
        return;
    }
    if (tg_gui_photo_disk_cache.items[at].bytes <=
        tg_gui_photo_disk_cache.total_bytes) {
        tg_gui_photo_disk_cache.total_bytes -=
            (unsigned long long)tg_gui_photo_disk_cache.items[at].bytes;
    } else {
        tg_gui_photo_disk_cache.total_bytes = 0ULL;
    }
    if (at + 1 < tg_gui_photo_disk_cache.count) {
        memmove(&tg_gui_photo_disk_cache.items[at],
                &tg_gui_photo_disk_cache.items[at + 1],
                (size_t)(tg_gui_photo_disk_cache.count - at - 1) *
                    sizeof(tg_gui_photo_disk_cache.items[0]));
        memmove(&tg_gui_photo_disk_cache.meta[at],
                &tg_gui_photo_disk_cache.meta[at + 1],
                (size_t)(tg_gui_photo_disk_cache.count - at - 1) *
                    sizeof(tg_gui_photo_disk_cache.meta[0]));
    }
    --tg_gui_photo_disk_cache.count;
}

static void tg_gui_photo_cache_scan_finish(void)
{
    if (tg_gui_photo_disk_cache.directory != (BPTR)0) {
        UnLock(tg_gui_photo_disk_cache.directory);
        tg_gui_photo_disk_cache.directory = (BPTR)0;
    }
    tg_gui_photo_disk_cache.scan_complete = 1;
    if (!tg_gui_photo_disk_cache.scan_failed &&
        tg_gui_photo_disk_cache.limit_bytes != 0ULL &&
        tg_gui_photo_disk_cache.total_bytes >
            tg_gui_photo_disk_cache.limit_bytes) {
        tg_gui_photo_disk_cache.prune_pending = 1;
    }
    if (tg_gui_log_is_enabled()) {
        char line[96];

        sprintf(line, "photo cache: scan done files=%d MB=%lu%s",
                tg_gui_photo_disk_cache.count,
                (unsigned long)(tg_gui_photo_disk_cache.total_bytes /
                                (1024ULL * 1024ULL)),
                tg_gui_photo_disk_cache.scan_failed ? " incomplete" : "");
        tg_gui_log(line);
    }
}

static void tg_gui_photo_cache_begin(unsigned long limit_mb)
{
    memset(&tg_gui_photo_disk_cache, 0, sizeof(tg_gui_photo_disk_cache));
    tg_gui_photo_disk_cache.active = 1;
    tg_gui_photo_disk_cache.limit_bytes =
        limit_mb == TG_GUI_PHOTO_CACHE_UNLIMITED_MB
            ? 0ULL
            : (unsigned long long)limit_mb * 1024ULL * 1024ULL;
    tg_gui_photo_disk_cache.directory =
        Lock((CONST_STRPTR)"photos", ACCESS_READ);
    if (tg_gui_photo_disk_cache.directory == (BPTR)0) {
        tg_gui_photo_cache_scan_finish();
    } else if (!TG_GUI_DOS_EXAMINE(tg_gui_photo_disk_cache.directory,
                                   &tg_gui_photo_disk_cache.fib) ||
               tg_gui_photo_disk_cache.fib.fib_DirEntryType < 0) {
        tg_gui_photo_disk_cache.scan_failed = 1;
        tg_gui_photo_cache_scan_finish();
    }
}

static void tg_gui_photo_cache_end(void)
{
    if (tg_gui_photo_disk_cache.clear_pending) {
        tg_gui_session_photo_cache_clear_finish();
    }
    if (tg_gui_photo_disk_cache.directory != (BPTR)0) {
        UnLock(tg_gui_photo_disk_cache.directory);
    }
    free(tg_gui_photo_disk_cache.items);
    free(tg_gui_photo_disk_cache.meta);
    memset(&tg_gui_photo_disk_cache, 0, sizeof(tg_gui_photo_disk_cache));
}

void tg_gui_window_photo_cache_file_changed(const char *path)
{
    BPTR lock;
    struct FileInfoBlock fib;
    const char *name;

    if (!tg_gui_photo_disk_cache.active || path == 0) {
        return;
    }
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)0) {
        tg_gui_window_photo_cache_file_removed(path);
        return;
    }
    memset(&fib, 0, sizeof(fib));
    if (TG_GUI_DOS_EXAMINE(lock, &fib) && fib.fib_DirEntryType < 0) {
        name = tg_gui_photo_cache_basename(path);
        (void)tg_gui_photo_cache_upsert(
            name, fib.fib_Size > 0 ? (unsigned long)fib.fib_Size : 0UL,
            &fib.fib_Date);
    }
    UnLock(lock);
}

void tg_gui_window_photo_cache_file_removed(const char *path)
{
    int at;

    if (!tg_gui_photo_disk_cache.active || path == 0) {
        return;
    }
    at = tg_gui_photo_cache_find(tg_gui_photo_cache_basename(path));
    if (at >= 0) {
        tg_gui_photo_cache_remove_at(at);
    }
}

static void tg_gui_photo_cache_visibility_changed(void)
{
    if (tg_gui_photo_disk_cache.active &&
        tg_gui_photo_disk_cache.scan_complete &&
        !tg_gui_photo_disk_cache.scan_failed &&
        !tg_gui_photo_disk_cache.clear_pending &&
        tg_gui_photo_disk_cache.limit_bytes != 0ULL &&
        tg_gui_photo_disk_cache.total_bytes >
            tg_gui_photo_disk_cache.limit_bytes) {
        tg_gui_photo_disk_cache.prune_pending = 1;
    }
}

static int tg_gui_photo_cache_pending(void)
{
    return tg_gui_photo_disk_cache.active &&
           (!tg_gui_photo_disk_cache.scan_complete ||
            tg_gui_photo_disk_cache.prune_pending ||
            tg_gui_photo_disk_cache.clear_pending);
}

static void tg_gui_photo_cache_update_protection(
    const tg_gui_photo_viewer *viewer)
{
    int i;

    for (i = 0; i < tg_gui_photo_disk_cache.count; ++i) {
        tg_gui_photo_cache_meta *meta;
        int protect;

        meta = &tg_gui_photo_disk_cache.meta[i];
        protect = meta->delete_failures >= 3U;
        if (!protect && (meta->id_hi != 0UL || meta->id_lo != 0UL)) {
            protect = tg_gui_photo_visible_priority(meta->id_hi,
                                                    meta->id_lo) >= 0;
            if (!protect && viewer != 0 && viewer->ctx.window != 0) {
                protect = viewer->slot.id_hi == meta->id_hi &&
                          viewer->slot.id_lo == meta->id_lo;
            }
        }
        tg_gui_photo_disk_cache.items[i].protected_entry = protect;
    }
}

static int tg_gui_photo_cache_delete_entry(int at)
{
    char path[144];

    if (at < 0 || at >= tg_gui_photo_disk_cache.count) {
        return 0;
    }
    sprintf(path, "photos/%s", tg_gui_photo_disk_cache.meta[at].name);
    if (remove(path) == 0) {
        return 1;
    }
    /* A file may have disappeared outside the client after the one startup
       scan. Treat that as already removed instead of retaining stale bytes. */
    {
        FILE *probe;

        probe = fopen(path, "rb");
        if (probe == 0) {
            return 1;
        }
        fclose(probe);
    }
    if (tg_gui_photo_disk_cache.meta[at].delete_failures < 255U) {
        ++tg_gui_photo_disk_cache.meta[at].delete_failures;
    }
    return 0;
}

static int tg_gui_photo_cache_scan_tick(void)
{
    int steps;

    if (tg_gui_photo_disk_cache.scan_complete) {
        return 0;
    }
    steps = 0;
    while (steps < TG_GUI_PHOTO_CACHE_SCAN_PER_TICK) {
        if (!TG_GUI_DOS_EXNEXT(tg_gui_photo_disk_cache.directory,
                               &tg_gui_photo_disk_cache.fib)) {
            tg_gui_photo_cache_scan_finish();
            break;
        }
        if (tg_gui_photo_disk_cache.fib.fib_DirEntryType < 0 &&
            !tg_gui_photo_cache_upsert(
                (const char *)tg_gui_photo_disk_cache.fib.fib_FileName,
                tg_gui_photo_disk_cache.fib.fib_Size > 0
                    ? (unsigned long)tg_gui_photo_disk_cache.fib.fib_Size
                    : 0UL,
                &tg_gui_photo_disk_cache.fib.fib_Date)) {
            tg_gui_photo_disk_cache.scan_failed = 1;
            tg_gui_photo_cache_scan_finish();
            break;
        }
        ++steps;
    }
    return steps > 0;
}

static int tg_gui_photo_cache_prune_tick(const tg_gui_photo_viewer *viewer)
{
    unsigned long planned;
    int deleted;
    int i;

    if (!tg_gui_photo_disk_cache.scan_complete ||
        !tg_gui_photo_disk_cache.prune_pending ||
        tg_gui_photo_disk_cache.clear_pending) {
        return 0;
    }
    tg_gui_photo_cache_update_protection(viewer);
    (void)tg_gui_photo_cache_prune_plan(
        tg_gui_photo_disk_cache.items, tg_gui_photo_disk_cache.count,
        tg_gui_photo_disk_cache.limit_bytes,
        TG_GUI_PHOTO_CACHE_DELETE_PER_TICK, &planned);
    if (planned == 0UL) {
        tg_gui_photo_disk_cache.prune_pending = 0;
        return 0;
    }
    deleted = 0;
    i = 0;
    while (i < tg_gui_photo_disk_cache.count &&
           deleted < TG_GUI_PHOTO_CACHE_DELETE_PER_TICK) {
        if (!tg_gui_photo_disk_cache.items[i].remove) {
            ++i;
            continue;
        }
        if (tg_gui_photo_cache_delete_entry(i)) {
            tg_gui_photo_cache_remove_at(i);
            ++deleted;
        } else {
            tg_gui_photo_disk_cache.items[i].remove = 0;
            ++i;
        }
    }
    tg_gui_photo_disk_cache.prune_pending =
        tg_gui_photo_disk_cache.limit_bytes != 0ULL &&
        tg_gui_photo_disk_cache.total_bytes >
            tg_gui_photo_disk_cache.limit_bytes;
    return deleted > 0;
}

static void tg_gui_photo_cache_cancel_slot_work(tg_gui_photo_slot *slot,
                                                int keep_frame)
{
    int has_frame;

    if (slot == 0) {
        return;
    }
    has_frame = slot->w > 0 && slot->h > 0 &&
                ((slot->rgb != 0 && slot->ready_rows >= slot->h) ||
                 (slot->pen != 0 && slot->pen_rows >= slot->h));
    if (!keep_frame || !has_frame) {
        tg_gui_photo_slot_clear(slot);
        return;
    }
    if (slot->canonical_file != 0) {
        fclose(slot->canonical_file);
        slot->canonical_file = 0;
    }
    tg_image_jpeg_decoder_destroy(slot->decoder);
    slot->decoder = 0;
    free(slot->jpeg);
    slot->jpeg = 0;
    slot->jpeg_len = 0UL;
    free(slot->stage_pen);
    slot->stage_pen = 0;
    free(slot->stage_rgb);
    slot->stage_rgb = 0;
    slot->stage_ready_rows = 0;
    slot->stage_pen_rows = 0;
    slot->decode_done = 0;
    slot->canonical_size = 0UL;
    slot->canonical_loaded = 0UL;
    slot->canonical_from_disk = 0;
    slot->state = 1;
    tg_gui_photo_pipeline_release(slot);
}

static void tg_gui_photo_cache_prepare_slots_for_clear(
    tg_gui_photo_viewer *viewer)
{
    int i;

    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        tg_gui_photo_slot *slot;
        int visible;

        slot = &tg_gui_photo_slots[i];
        visible = slot->state != 0 &&
                  tg_gui_photo_visible_priority(slot->id_hi,
                                                slot->id_lo) >= 0;
        if (!visible) {
            tg_gui_photo_slot_clear(slot);
        } else if (slot->state == 2) {
            tg_gui_photo_cache_cancel_slot_work(slot, 1);
        }
    }
    tg_gui_photo_request_count = 0;
    if (viewer != 0 && viewer->ctx.window != 0 && viewer->slot.state == 2) {
        unsigned long id_hi;
        unsigned long id_lo;

        id_hi = viewer->slot.id_hi;
        id_lo = viewer->slot.id_lo;
        tg_gui_photo_cache_cancel_slot_work(&viewer->slot, 1);
        if (viewer->slot.state == 0) {
            viewer->slot.id_hi = id_hi;
            viewer->slot.id_lo = id_lo;
        }
    }
    tg_gui_photo_decode_gate_reset(&tg_gui_photo_decode_pipeline);
}

static void tg_gui_photo_cache_request_clear(tg_gui_state *state,
                                             tg_gui_photo_viewer *viewer)
{
    int i;

    if (!tg_gui_photo_disk_cache.active ||
        tg_gui_photo_disk_cache.clear_pending) {
        return;
    }
    tg_gui_session_photo_cache_clear_prepare();
    tg_gui_photo_cache_prepare_slots_for_clear(viewer);
    for (i = 0; i < tg_gui_photo_disk_cache.count; ++i) {
        tg_gui_photo_disk_cache.meta[i].delete_failures = 0;
    }
    tg_gui_photo_disk_cache.clear_pending = 1;
    tg_gui_photo_disk_cache.prune_pending = 0;
    tg_gui_photo_disk_cache.clear_files = 0UL;
    tg_gui_photo_disk_cache.clear_bytes = 0ULL;
    tg_gui_window_copy(state->status, sizeof(state->status),
                       "Clearing photo cache...");
}

static int tg_gui_photo_cache_clear_tick(tg_gui_state *state)
{
    int deleted;
    int i;
    int retryable;

    if (!tg_gui_photo_disk_cache.clear_pending ||
        !tg_gui_photo_disk_cache.scan_complete) {
        return 0;
    }
    deleted = 0;
    i = 0;
    while (i < tg_gui_photo_disk_cache.count &&
           deleted < TG_GUI_PHOTO_CACHE_DELETE_PER_TICK) {
        unsigned long bytes;

        bytes = tg_gui_photo_disk_cache.items[i].bytes;
        if (tg_gui_photo_cache_delete_entry(i)) {
            ++tg_gui_photo_disk_cache.clear_files;
            tg_gui_photo_disk_cache.clear_bytes +=
                (unsigned long long)bytes;
            tg_gui_photo_cache_remove_at(i);
            ++deleted;
        } else {
            ++i;
        }
    }
    retryable = 0;
    for (i = 0; i < tg_gui_photo_disk_cache.count; ++i) {
        if (tg_gui_photo_disk_cache.meta[i].delete_failures < 3U) {
            retryable = 1;
            break;
        }
    }
    if (tg_gui_photo_disk_cache.count == 0 || !retryable) {
        char line[64];

        if (tg_gui_photo_disk_cache.scan_failed) {
            strcpy(line, "Photo cache clear incomplete: directory scan failed");
        } else if (tg_gui_photo_disk_cache.count == 0) {
            unsigned long mb;
            unsigned long tenth;
            unsigned long long remain;

            mb = (unsigned long)(tg_gui_photo_disk_cache.clear_bytes /
                                 (1024ULL * 1024ULL));
            remain = tg_gui_photo_disk_cache.clear_bytes %
                     (1024ULL * 1024ULL);
            tenth = (unsigned long)((remain * 10ULL + 512ULL * 1024ULL) /
                                    (1024ULL * 1024ULL));
            if (tenth >= 10UL) {
                ++mb;
                tenth = 0UL;
            }
            sprintf(line, "Photo cache cleared: %lu files, %lu.%lu MB",
                    tg_gui_photo_disk_cache.clear_files, mb, tenth);
        } else {
            sprintf(line, "Photo cache clear incomplete: %d files busy",
                    tg_gui_photo_disk_cache.count);
        }
        tg_gui_window_copy(state->status, sizeof(state->status), line);
        tg_gui_photo_disk_cache.clear_pending = 0;
        tg_gui_photo_disk_cache.prune_pending =
            !tg_gui_photo_disk_cache.scan_failed &&
            tg_gui_photo_disk_cache.limit_bytes != 0ULL &&
            tg_gui_photo_disk_cache.total_bytes >
                tg_gui_photo_disk_cache.limit_bytes;
        tg_gui_session_photo_cache_clear_finish();
        return 1;
    }
    return 0;
}

static int tg_gui_photo_cache_maintenance_tick(
    tg_gui_state *state, const tg_gui_photo_viewer *viewer)
{
    int status_changed;

    status_changed = 0;
    if (!tg_gui_photo_disk_cache.scan_complete) {
        (void)tg_gui_photo_cache_scan_tick();
    }
    if (tg_gui_photo_disk_cache.clear_pending) {
        status_changed = tg_gui_photo_cache_clear_tick(state);
    } else {
        (void)tg_gui_photo_cache_prune_tick(viewer);
    }
    return status_changed;
}

static void tg_gui_photo_cache_set_limit(unsigned long limit_mb)
{
    tg_gui_photo_disk_cache.limit_bytes =
        limit_mb == TG_GUI_PHOTO_CACHE_UNLIMITED_MB
            ? 0ULL
            : (unsigned long long)limit_mb * 1024ULL * 1024ULL;
    tg_gui_photo_disk_cache.prune_pending =
        tg_gui_photo_disk_cache.scan_complete &&
        !tg_gui_photo_disk_cache.scan_failed &&
        tg_gui_photo_disk_cache.limit_bytes != 0ULL &&
        tg_gui_photo_disk_cache.total_bytes >
            tg_gui_photo_disk_cache.limit_bytes;
}

/* A transient fetch failure clears the session queue but must not leave the
   already painted placeholder inert forever. Re-offer current viewport demand
   on later photo ticks; duplicate/active/cache checks keep this O(visible). */
static void tg_gui_photo_reoffer_visible(void)
{
    int i;

    for (i = 0; i < tg_gui_photo_visible_count; ++i) {
        (void)tg_gui_session_request_inline_photo(
            tg_gui_photo_visible_ids[i].id_hi,
            tg_gui_photo_visible_ids[i].id_lo);
    }
}

static int tg_gui_photo_decode_pending(const tg_gui_photo_viewer *viewer)
{
    int i;

    if (tg_gui_photo_request_count > 0) {
        return 1;
    }
    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        if (tg_gui_photo_slots[i].state == 2) {
            return 1;
        }
    }
    return viewer != 0 && viewer->ctx.window != 0 &&
           (viewer->slot.id_hi != 0UL || viewer->slot.id_lo != 0UL) &&
           (viewer->slot.state == 0 || viewer->slot.state == 2 ||
            viewer->slot.preview_only);
}

static void tg_gui_photo_slot_touch(tg_gui_photo_slot *slot)
{
    if (slot == 0) {
        return;
    }
    ++tg_gui_photo_use_clock;
    if (tg_gui_photo_use_clock == 0UL) {
        tg_gui_photo_use_clock = 1UL;
    }
    slot->last_use = tg_gui_photo_use_clock;
}

static tg_gui_photo_slot *tg_gui_photo_slot_claim(unsigned long id_hi,
                                                  unsigned long id_lo)
{
    tg_gui_photo_slot *slot;
    int states[TG_GUI_PHOTO_SLOTS];
    unsigned char visible[TG_GUI_PHOTO_SLOTS];
    unsigned long last_use[TG_GUI_PHOTO_SLOTS];
    int i;
    int chosen;

    slot = 0;
    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        if (tg_gui_photo_slots[i].state != 0 &&
            tg_gui_photo_slots[i].id_hi == id_hi &&
            tg_gui_photo_slots[i].id_lo == id_lo) {
            tg_gui_photo_slot_touch(&tg_gui_photo_slots[i]);
            return &tg_gui_photo_slots[i];
        }
    }
    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        states[i] = tg_gui_photo_slots[i].state;
        visible[i] = (unsigned char)(
            states[i] > 0 &&
            tg_gui_photo_visible_priority(tg_gui_photo_slots[i].id_hi,
                                          tg_gui_photo_slots[i].id_lo) >= 0);
        last_use[i] = tg_gui_photo_slots[i].last_use;
    }
    chosen = tg_gui_photo_cache_choose_slot(states, visible, last_use,
                                            TG_GUI_PHOTO_SLOTS);
    if (chosen < 0) {
        return 0;
    }
    slot = &tg_gui_photo_slots[chosen];
    tg_gui_photo_slot_clear(slot);
    slot->id_hi = id_hi;
    slot->id_lo = id_lo;
    tg_gui_photo_slot_touch(slot);
    return slot;
}

static tg_gui_photo_slot *tg_gui_photo_slot_find(unsigned long id_hi,
                                                 unsigned long id_lo)
{
    int i;

    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        if (tg_gui_photo_slots[i].state != 0 &&
            tg_gui_photo_slots[i].id_hi == id_hi &&
            tg_gui_photo_slots[i].id_lo == id_lo) {
            return &tg_gui_photo_slots[i];
        }
    }
    return 0;
}

static void tg_gui_photo_request_offer(unsigned long id_hi,
                                       unsigned long id_lo,
                                       unsigned long source_w,
                                       unsigned long source_h)
{
    tg_gui_photo_slot *existing;
    int i;

    existing = tg_gui_photo_slot_find(id_hi, id_lo);
    if ((id_hi == 0UL && id_lo == 0UL) || source_w == 0UL || source_h == 0UL ||
        (existing != 0 && !existing->preview_only)) {
        return;
    }
    for (i = 0; i < tg_gui_photo_request_count; ++i) {
        if (tg_gui_photo_requests[i].id_hi == id_hi &&
            tg_gui_photo_requests[i].id_lo == id_lo) {
            tg_gui_photo_requests[i].source_w = source_w;
            tg_gui_photo_requests[i].source_h = source_h;
            return;
        }
    }
    if (tg_gui_photo_request_count >= TG_GUI_PHOTO_REQUESTS) {
        for (i = 1; i < TG_GUI_PHOTO_REQUESTS; ++i) {
            tg_gui_photo_requests[i - 1] = tg_gui_photo_requests[i];
        }
        tg_gui_photo_request_count = TG_GUI_PHOTO_REQUESTS - 1;
    }
    tg_gui_photo_requests[tg_gui_photo_request_count].id_hi = id_hi;
    tg_gui_photo_requests[tg_gui_photo_request_count].id_lo = id_lo;
    tg_gui_photo_requests[tg_gui_photo_request_count].source_w = source_w;
    tg_gui_photo_requests[tg_gui_photo_request_count].source_h = source_h;
    ++tg_gui_photo_request_count;
}

static void tg_gui_photo_request_remove_first(void)
{
    int i;

    if (tg_gui_photo_request_count <= 0) {
        return;
    }
    for (i = 1; i < tg_gui_photo_request_count; ++i) {
        tg_gui_photo_requests[i - 1] = tg_gui_photo_requests[i];
    }
    --tg_gui_photo_request_count;
}

static int tg_gui_photo_map_pen_rows(tg_gui_amiga_ctx *ctx,
                                     tg_gui_photo_slot *slot,
                                     const unsigned char *rgb,
                                     int width,
                                     int height,
                                     unsigned char **pen_grid,
                                     int ready_rows,
                                     int *mapped_rows,
                                     int max_rows)
{
    int end_row;
    int y;

    if (slot == 0 || rgb == 0 || pen_grid == 0 || mapped_rows == 0 ||
        width <= 0 || height <= 0 || max_rows <= 0) {
        return 0;
    }
    if (*pen_grid == 0) {
        unsigned long pixels;

        pixels = (unsigned long)width * (unsigned long)height;
        *pen_grid = (unsigned char *)malloc((size_t)pixels);
        if (*pen_grid == 0) {
            tg_gui_photo_diag("photo: pen map wait (no memory)");
            return 0;
        }
    }
    end_row = ready_rows;
    if (end_row > height) {
        end_row = height;
    }
    if (end_row > *mapped_rows + max_rows) {
        end_row = *mapped_rows + max_rows;
    }
    for (y = *mapped_rows; y < end_row; ++y) {
        int x;

        for (x = 0; x < width; ++x) {
            LONG p;

            p = tg_gui_photo_pen_for(
                ctx,
                rgb + ((((unsigned long)y * (unsigned long)width) +
                        (unsigned long)x) * 3UL), x, y);
            if (p == -1) {
                tg_gui_photo_diag("photo: pen map fail");
                return 0;
            }
            (*pen_grid)[(unsigned long)y * (unsigned long)width +
                        (unsigned long)x] = (unsigned char)p;
        }
    }
    if (end_row > *mapped_rows) {
        *mapped_rows = end_row;
        return 1;
    }
    return 0;
}

static int tg_gui_photo_next_quality_scale(const tg_gui_photo_slot *slot)
{
    if (slot == 0 || slot->pass_scale <= slot->final_scale) {
        return TG_IMAGE_JPEG_SCALE_AUTO;
    }
    if (slot->pass_scale == 3 && slot->final_scale < 2) {
        return 2; /* 1/8 -> 1/4 before the finest useful scale */
    }
    return slot->final_scale;
}

static int tg_gui_photo_begin_quality_pass(tg_gui_photo_slot *slot,
                                           int decode_cap,
                                           int requested_scale,
                                           int bilinear_upscale,
                                           int *actual_scale,
                                           int *decode_rc)
{
    unsigned long pixels;

    if (slot == 0 || slot->jpeg == 0 ||
        slot->decode_w <= 0 || slot->decode_h <= 0) {
        return 0;
    }
    pixels = (unsigned long)slot->decode_w * (unsigned long)slot->decode_h;
    free(slot->stage_pen);
    slot->stage_pen = 0;
    slot->stage_pen_rows = 0;
    slot->stage_ready_rows = 0;
    slot->decode_done = 0;
    if (slot->stage_rgb == 0) {
        slot->stage_rgb = (unsigned char *)calloc((size_t)pixels, 3U);
    } else {
        memset(slot->stage_rgb, 0, (size_t)pixels * 3U);
    }
    if (slot->stage_rgb == 0) {
        if (decode_rc != 0) {
            *decode_rc = 2;
        }
        return 0;
    }
    slot->decoder = bilinear_upscale
        ? tg_image_jpeg_decoder_begin_scale_bilinear(
              slot->jpeg, slot->jpeg_len, slot->stage_rgb,
              slot->decode_w, slot->decode_h, decode_cap, requested_scale,
              actual_scale, decode_rc)
        : tg_image_jpeg_decoder_begin_scale(
              slot->jpeg, slot->jpeg_len, slot->stage_rgb,
              slot->decode_w, slot->decode_h, decode_cap, requested_scale,
              actual_scale, decode_rc);
    return slot->decoder != 0;
}

static int tg_gui_photo_begin_quality_sequence(tg_gui_photo_slot *slot,
                                               int decode_cap,
                                               int *decode_rc)
{
    int actual_scale;

    actual_scale = TG_IMAGE_JPEG_SCALE_AUTO;
    if (!tg_gui_photo_begin_quality_pass(
            slot, decode_cap, TG_IMAGE_JPEG_SCALE_AUTO,
            1, &actual_scale, decode_rc)) {
        return 0;
    }
    slot->final_scale = actual_scale;
    slot->pass_scale = actual_scale;
    if (actual_scale < 3) {
        tg_image_jpeg_decoder_destroy(slot->decoder);
        slot->decoder = 0;
        if (!tg_gui_photo_begin_quality_pass(slot, decode_cap, 3, 0,
                                             &actual_scale, decode_rc)) {
            return 0;
        }
        slot->pass_scale = actual_scale;
    }
    return 1;
}

static int tg_gui_photo_finish_quality_sequence(tg_gui_photo_slot *slot,
                                                int viewer_scope)
{
    int scope;

    scope = viewer_scope ? TG_GUI_PHOTO_SCOPE_VIEWER
                         : TG_GUI_PHOTO_SCOPE_INLINE;
    if (slot == 0 || !tg_gui_photo_pipeline_owns(slot, scope)) {
        return 0;
    }
    slot->quality_done = 1;
    slot->state = 1;
    free(slot->jpeg);
    slot->jpeg = 0;
    slot->jpeg_len = 0UL;
    tg_gui_photo_pipeline_release(slot);
    return 1;
}

static void tg_gui_photo_canonical_load_abort(tg_gui_photo_slot *slot)
{
    unsigned long id_hi;
    unsigned long id_lo;
    char path[64];
    int large;

    if (slot == 0) {
        return;
    }
    id_hi = slot->id_hi;
    id_lo = slot->id_lo;
    large = slot->canonical_large;
    if (slot->canonical_file != 0) {
        fclose(slot->canonical_file);
        slot->canonical_file = 0;
    }
    if (tg_gui_session_photo_canonical_cache_path(
            path, sizeof(path), id_hi, id_lo, large) == 0) {
        (void)remove(path);
        tg_gui_window_photo_cache_file_removed(path);
    }
    free(slot->stage_rgb);
    slot->stage_rgb = 0;
    free(slot->stage_pen);
    slot->stage_pen = 0;
    slot->stage_ready_rows = 0;
    slot->stage_pen_rows = 0;
    slot->decode_done = 0;
    slot->decode_w = 0;
    slot->decode_h = 0;
    slot->canonical_size = 0UL;
    slot->canonical_loaded = 0UL;
    slot->canonical_from_disk = 0;
    if (slot->preview_only && (slot->rgb != 0 || slot->pen != 0)) {
        slot->state = 1;
        tg_gui_photo_pipeline_release(slot);
    } else {
        tg_gui_photo_slot_clear(slot);
        slot->id_hi = id_hi;
        slot->id_lo = id_lo;
    }
    tg_gui_photo_diag("photo: canonical cache rejected");
}

static int tg_gui_photo_canonical_load_start(tg_gui_photo_slot *slot,
                                             int width, int height,
                                             int large)
{
    FILE *file;
    unsigned long payload_size;
    char path[64];

    if (slot == 0 || width <= 0 || height <= 0 ||
        (slot->state != 0 &&
         !(slot->preview_only && slot->state == 1)) ||
        tg_gui_session_photo_canonical_cache_path(
            path, sizeof(path), slot->id_hi, slot->id_lo, large) != 0) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == 0) {
        return 0;
    }
    if (tg_image_canonical_cache_prepare(
            file, width, height, &payload_size) != 0) {
        fclose(file);
        (void)remove(path);
        tg_gui_window_photo_cache_file_removed(path);
        tg_gui_photo_diag("photo: stale canonical cache removed");
        return 0;
    }
    free(slot->stage_rgb);
    slot->stage_rgb = (unsigned char *)malloc((size_t)payload_size);
    if (slot->stage_rgb == 0) {
        fclose(file);
        tg_gui_photo_diag("photo: canonical cache wait (no memory)");
        return 0;
    }
    free(slot->stage_pen);
    slot->stage_pen = 0;
    slot->stage_pen_rows = 0;
    slot->stage_ready_rows = 0;
    slot->decode_done = 0;
    slot->decode_w = width;
    slot->decode_h = height;
    slot->canonical_file = file;
    slot->canonical_size = payload_size;
    slot->canonical_loaded = 0UL;
    slot->canonical_large = large ? 1 : 0;
    slot->canonical_from_disk = 1;
    slot->pass_scale = 0;
    slot->final_scale = 0;
    slot->state = 2;
    tg_gui_photo_diag(large
        ? "photo: viewer canonical cache load begin"
        : "photo: canonical cache load begin");
    return 1;
}

static int tg_gui_photo_canonical_load_tick(tg_gui_photo_slot *slot,
                                            unsigned long cache_budget)
{
    unsigned long remain;
    unsigned long chunk;
    unsigned long got;

    if (slot == 0 || slot->canonical_file == 0 || slot->stage_rgb == 0 ||
        slot->canonical_loaded > slot->canonical_size || cache_budget == 0UL) {
        return -1;
    }
    remain = slot->canonical_size - slot->canonical_loaded;
    chunk = remain > cache_budget ? cache_budget : remain;
    got = (unsigned long)fread(
        slot->stage_rgb + slot->canonical_loaded, 1, (size_t)chunk,
        slot->canonical_file);
    if (got != chunk) {
        return -1;
    }
    slot->canonical_loaded += got;
    if (slot->canonical_loaded < slot->canonical_size) {
        return 0;
    }
    fclose(slot->canonical_file);
    slot->canonical_file = 0;
    slot->stage_ready_rows = slot->decode_h;
    slot->decode_done = 1;
    return 1;
}

/* Publish a complete pass atomically. The previous quality remains drawable
   while entropy decode and pen mapping prepare the next hidden frame. */
static int tg_gui_photo_commit_quality_pass(tg_gui_amiga_ctx *ctx,
                                            tg_gui_photo_slot *slot,
                                            int decode_cap,
                                            int viewer_scope)
{
    int next_scale;
    int actual_scale;
    int decode_rc;
    char line[80];

    int scope;

    scope = viewer_scope ? TG_GUI_PHOTO_SCOPE_VIEWER
                         : TG_GUI_PHOTO_SCOPE_INLINE;
    if (ctx == 0 || slot == 0 || slot->stage_rgb == 0 ||
        !tg_gui_photo_pipeline_owns(slot, scope)) {
        return 0;
    }
    next_scale = tg_gui_photo_next_quality_scale(slot);
    if (next_scale == TG_IMAGE_JPEG_SCALE_AUTO &&
        !slot->canonical_from_disk) {
        char cache_path[64];

        if (tg_gui_session_photo_canonical_cache_path(
                cache_path, sizeof(cache_path), slot->id_hi, slot->id_lo,
                viewer_scope) == 0) {
            if (tg_image_canonical_cache_write(
                    cache_path, slot->stage_rgb,
                    slot->decode_w, slot->decode_h) == 0) {
                tg_gui_window_photo_cache_file_changed(cache_path);
                tg_gui_photo_diag(viewer_scope
                    ? "photo: viewer canonical cache saved"
                    : "photo: canonical cache saved");
            } else {
                /* The writer may already have removed an older target before
                   its final rename failed. Re-stat so the byte catalog follows
                   the filesystem in both failure modes. */
                tg_gui_window_photo_cache_file_changed(cache_path);
                tg_gui_photo_diag("photo: canonical cache write failed");
            }
        }
    }
    if (ctx->photo_truecolor) {
        free(slot->rgb);
        slot->rgb = slot->stage_rgb;
        slot->stage_rgb = 0;
        slot->ready_rows = slot->decode_h;
        free(slot->pen);
        slot->pen = 0;
        slot->pen_rows = 0;
    } else {
        if (slot->stage_pen == 0 ||
            slot->stage_pen_rows < slot->decode_h) {
            return 0;
        }
        free(slot->pen);
        slot->pen = slot->stage_pen;
        slot->stage_pen = 0;
        slot->pen_rows = slot->decode_h;
        free(slot->rgb);
        slot->rgb = 0;
        slot->ready_rows = 0;
        free(slot->stage_rgb);
        slot->stage_rgb = 0;
    }
    slot->w = slot->decode_w;
    slot->h = slot->decode_h;
    slot->preview_only = 0;
    slot->canonical_from_disk = 0;
    slot->canonical_size = 0UL;
    slot->canonical_loaded = 0UL;
    slot->stage_ready_rows = 0;
    slot->stage_pen_rows = 0;
    slot->decode_done = 0;
    slot->render_logged = 0;
    if (tg_gui_log_is_enabled()) {
        sprintf(line, "photo: %squality pass 1/%d",
                viewer_scope ? "viewer " : "", 1 << slot->pass_scale);
        tg_gui_log(line);
    }
    if (next_scale != TG_IMAGE_JPEG_SCALE_AUTO) {
        actual_scale = TG_IMAGE_JPEG_SCALE_AUTO;
        if (tg_gui_photo_begin_quality_pass(
                slot, decode_cap, next_scale,
                next_scale == slot->final_scale,
                                            &actual_scale, &decode_rc)) {
            slot->pass_scale = actual_scale;
            slot->state = 2;
            return 1;
        }
        tg_gui_photo_diag(viewer_scope
            ? "photo: viewer detail pass skipped"
            : "photo: detail pass skipped");
    }
    tg_gui_photo_finish_quality_sequence(slot, viewer_scope);
    tg_gui_photo_diag(viewer_scope
        ? "photo: viewer decode done"
        : "photo: decode done");
    return 1;
}

/* Advance one hidden quality pass. A positive return means a whole frame was
   published, zero means more bounded work is pending, and -1 is JPEG failure. */
static int tg_gui_photo_quality_tick(tg_gui_amiga_ctx *ctx,
                                     tg_gui_photo_slot *slot,
                                     unsigned int mcu_budget,
                                     int pen_row_budget,
                                     unsigned long cache_budget,
                                     int decode_cap,
                                     int viewer_scope,
                                     int *decode_rc,
                                     int *work_kind)
{
    int ready_rows;
    int step_rc;

    int scope;

    if (work_kind != 0) {
        *work_kind = TG_GUI_PHOTO_WORK_NONE;
    }
    scope = viewer_scope ? TG_GUI_PHOTO_SCOPE_VIEWER
                         : TG_GUI_PHOTO_SCOPE_INLINE;
    if (ctx == 0 || slot == 0 || mcu_budget == 0U || pen_row_budget <= 0 ||
        !tg_gui_photo_pipeline_owns(slot, scope)) {
        return 0;
    }
    /* CyberGraphX may fail its runtime self-check after the coarse RGB pass.
       Convert that already visible pass fully before exposing any pen rows. */
    if (!ctx->photo_truecolor && slot->rgb != 0 &&
        slot->pen_rows < slot->h) {
        int display_pen_budget;

        if (work_kind != 0) {
            *work_kind = TG_GUI_PHOTO_WORK_REPLAY;
        }
        display_pen_budget = pen_row_budget;
        if (slot->preview_only && display_pen_budget < slot->h) {
            /* The stripped frame is deliberately small: map it in one idle
               turn so paletted systems get the same immediate first frame as
               truecolor systems. Full JPEG passes remain sliced. */
            display_pen_budget = slot->h;
        }
        (void)tg_gui_photo_map_pen_rows(
            ctx, slot, slot->rgb, slot->w, slot->h,
            &slot->pen, slot->h,
            &slot->pen_rows, display_pen_budget);
        if (slot->pen_rows >= slot->h) {
            free(slot->rgb);
            slot->rgb = 0;
            slot->ready_rows = 0;
            slot->render_logged = 0;
            if (slot->quality_done || slot->preview_only) {
                slot->state = 1;
                tg_gui_photo_pipeline_release(slot);
            }
            return 1;
        }
        return 0;
    }
    if (slot->canonical_file != 0) {
        if (work_kind != 0) {
            *work_kind = TG_GUI_PHOTO_WORK_CACHE;
        }
        step_rc = tg_gui_photo_canonical_load_tick(slot, cache_budget);
        if (step_rc < 0) {
            tg_gui_photo_canonical_load_abort(slot);
            return 1;
        }
        if (step_rc == 0) {
            return 0;
        }
        /* Publish/map on a separate measured turn. A completed disk read must
           never make the replay controller look like cache I/O, or vice versa. */
        return 0;
    }
    if (slot->decoder != 0) {
        if (work_kind != 0) {
            *work_kind = TG_GUI_PHOTO_WORK_DECODE;
        }
        ready_rows = slot->stage_ready_rows;
        step_rc = tg_image_jpeg_decoder_step(
            slot->decoder, mcu_budget, &ready_rows, decode_rc);
        if (step_rc < 0) {
            return -1;
        }
        slot->stage_ready_rows = ready_rows;
        if (step_rc == 1) {
            tg_image_jpeg_decoder_destroy(slot->decoder);
            slot->decoder = 0;
            slot->stage_ready_rows = slot->decode_h;
            slot->decode_done = 1;
        }
        /* Mapping and atomic publish have their own pace controller. Even a
           decoder that completed this slice yields before either cost. */
        return 0;
    }
    if (slot->decode_done && !ctx->photo_truecolor) {
        if (work_kind != 0) {
            *work_kind = TG_GUI_PHOTO_WORK_REPLAY;
        }
        (void)tg_gui_photo_map_pen_rows(
            ctx, slot, slot->stage_rgb,
            slot->decode_w, slot->decode_h, &slot->stage_pen,
            slot->stage_ready_rows, &slot->stage_pen_rows,
            pen_row_budget);
    }
    if (slot->decode_done &&
        (ctx->photo_truecolor ||
         slot->stage_pen_rows >= slot->decode_h)) {
        if (work_kind != 0) {
            *work_kind = TG_GUI_PHOTO_WORK_REPLAY;
        }
        return tg_gui_photo_commit_quality_pass(
            ctx, slot, decode_cap, viewer_scope);
    }
    return 0;
}

static void tg_gui_photo_decode_reject(tg_gui_photo_slot *slot, int decode_rc)
{
    unsigned long id_hi;
    unsigned long id_lo;
    char line[64];

    if (slot == 0) {
        return;
    }
    id_hi = slot->id_hi;
    id_lo = slot->id_lo;
    if (tg_gui_log_is_enabled()) {
        sprintf(line, "photo: decode fail rc=%d", decode_rc);
        tg_gui_log(line);
    }
    tg_gui_session_photo_decode_failed(id_hi, id_lo);
    if (slot->preview_only && (slot->rgb != 0 || slot->pen != 0)) {
        tg_image_jpeg_decoder_destroy(slot->decoder);
        slot->decoder = 0;
        free(slot->jpeg);
        slot->jpeg = 0;
        slot->jpeg_len = 0UL;
        free(slot->stage_pen);
        slot->stage_pen = 0;
        free(slot->stage_rgb);
        slot->stage_rgb = 0;
        slot->stage_ready_rows = 0;
        slot->stage_pen_rows = 0;
        slot->decode_done = 0;
        slot->decode_w = 0;
        slot->decode_h = 0;
        slot->state = 1;
        tg_gui_photo_pipeline_release(slot);
        return;
    }
    tg_gui_photo_slot_clear(slot);
    slot->id_hi = id_hi;
    slot->id_lo = id_lo;
    slot->state = -1;
}

static int tg_gui_photo_preview_start(tg_gui_amiga_ctx *ctx,
                                      tg_gui_photo_slot *slot,
                                      unsigned long source_w,
                                      unsigned long source_h,
                                      int edge_cap,
                                      int viewer_scope)
{
    unsigned char jpeg[900];
    unsigned char *rgb;
    FILE *file;
    char path[64];
    long flen;
    unsigned long got;
    unsigned long pixels;
    int w;
    int h;

    if (ctx == 0 || slot == 0 || slot->state != 0 || source_w == 0UL ||
        source_h == 0UL || edge_cap <= 0 ||
        tg_gui_session_photo_thumb_cache_path(
            path, sizeof(path), slot->id_hi, slot->id_lo) != 0) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == 0 || fseek(file, 0L, SEEK_END) != 0) {
        if (file != 0) {
            fclose(file);
        }
        return 0;
    }
    flen = ftell(file);
    if (flen <= 0L || (unsigned long)flen > sizeof(jpeg) ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        (void)remove(path);
        tg_gui_window_photo_cache_file_removed(path);
        return 0;
    }
    got = (unsigned long)fread(jpeg, 1, (size_t)flen, file);
    fclose(file);
    if (got != (unsigned long)flen ||
        tg_image_canonical_size(source_w, source_h, edge_cap, &w, &h) != 0) {
        (void)remove(path);
        tg_gui_window_photo_cache_file_removed(path);
        return 0;
    }
    pixels = (unsigned long)w * (unsigned long)h;
    rgb = (unsigned char *)malloc((size_t)pixels * 3U);
    if (rgb == 0) {
        return 0;
    }
    if (tg_avatar_decode_jpeg(jpeg, got, rgb, w, h) != 0) {
        free(rgb);
        (void)remove(path);
        tg_gui_window_photo_cache_file_removed(path);
        tg_gui_photo_diag("photo: stripped preview decode failed");
        return 0;
    }
    slot->rgb = rgb;
    slot->w = w;
    slot->h = h;
    slot->ready_rows = h;
    slot->preview_only = 1;
    slot->quality_done = 0;
    slot->state = ctx->photo_truecolor ? 1 : 2;
    slot->render_logged = 0;
    tg_gui_photo_diag(viewer_scope
        ? "photo: viewer stripped preview ready"
        : "photo: stripped preview ready");
    return 1;
}

static int tg_gui_photo_preview_ensure(tg_gui_amiga_ctx *ctx,
                                       tg_gui_photo_slot *slot,
                                       unsigned long source_w,
                                       unsigned long source_h,
                                       int edge_cap,
                                       int viewer_scope,
                                       int *changed)
{
    if (changed != 0) {
        *changed = 0;
    }
    if (ctx == 0 || slot == 0) {
        return 0;
    }
    if (slot->state == 0) {
        if (!tg_gui_photo_preview_start(
                ctx, slot, source_w, source_h, edge_cap, viewer_scope)) {
            return 0;
        }
        if (changed != 0) {
            *changed = 1;
        }
    }
    if (!slot->preview_only) {
        return 0;
    }
    if (!ctx->photo_truecolor && slot->rgb != 0 &&
        slot->pen_rows < slot->h) {
        (void)tg_gui_photo_map_pen_rows(
            ctx, slot, slot->rgb, slot->w, slot->h,
            &slot->pen, slot->h, &slot->pen_rows, slot->h);
        if (slot->pen_rows >= slot->h) {
            free(slot->rgb);
            slot->rgb = 0;
            slot->ready_rows = 0;
            slot->state = 1;
            slot->render_logged = 0;
            if (changed != 0) {
                *changed = 1;
            }
        }
    }
    return ctx->photo_truecolor
        ? slot->state == 1 && slot->rgb != 0 && slot->ready_rows >= slot->h
        : slot->state == 1 && slot->pen != 0 && slot->pen_rows >= slot->h;
}

typedef struct tg_gui_photo_preview_drain {
    tg_gui_amiga_ctx *ctx;
    int changed;
} tg_gui_photo_preview_drain;

static int tg_gui_photo_preview_prepare_request(void *context, int index)
{
    tg_gui_photo_preview_drain *drain;
    tg_gui_photo_request *request;
    tg_gui_photo_slot *slot;
    int changed;
    int ready;

    drain = (tg_gui_photo_preview_drain *)context;
    if (drain == 0 || drain->ctx == 0 || index < 0 ||
        index >= tg_gui_photo_request_count) {
        return 0;
    }
    request = &tg_gui_photo_requests[index];
    slot = tg_gui_photo_slot_find(request->id_hi, request->id_lo);
    if (slot == 0) {
        slot = tg_gui_photo_slot_claim(request->id_hi, request->id_lo);
    }
    if (slot == 0) {
        return 0;
    }
    changed = 0;
    ready = tg_gui_photo_preview_ensure(
        drain->ctx, slot, request->source_w, request->source_h,
        TG_GUI_PHOTO_PREVIEW_CAP, 0, &changed);
    if (changed) {
        drain->changed = 1;
    }
    return ready;
}

static int tg_gui_photo_preview_drain_visible(tg_gui_amiga_ctx *ctx)
{
    tg_gui_photo_preview_drain drain;

    if (ctx == 0 || ctx->photo_resize_active ||
        tg_gui_photo_request_count <= 0) {
        return 0;
    }
    drain.ctx = ctx;
    drain.changed = 0;
    (void)tg_gui_photo_preview_prepare_all(
        tg_gui_photo_request_count,
        tg_gui_photo_preview_prepare_request, &drain);
    return drain.changed;
}

static int tg_gui_photo_decode_start(tg_gui_amiga_ctx *ctx)
{
    tg_gui_photo_request request;
    tg_gui_photo_slot *slot;
    FILE *f;
    char path[64];
    long flen;
    unsigned long got;
    int canonical_w;
    int canonical_h;
    int decode_rc;
    int preview_ready;
    if (tg_gui_photo_request_count <= 0 ||
        tg_gui_photo_decode_pipeline.owner != 0) {
        return 0;
    }
    request = tg_gui_photo_requests[0];
    canonical_w = canonical_h = 0;
    if (tg_image_canonical_size(request.source_w, request.source_h,
                                TG_GUI_PHOTO_CANONICAL_CAP,
                                &canonical_w, &canonical_h) != 0) {
        tg_gui_photo_request_remove_first();
        tg_gui_photo_diag("photo: decode fail geometry");
        return 0;
    }
    slot = tg_gui_photo_slot_find(request.id_hi, request.id_lo);
    if (slot != 0 && slot->preview_only && slot->state != 1) {
        return 0;
    }
    if (slot == 0) {
        slot = tg_gui_photo_slot_claim(request.id_hi, request.id_lo);
    }
    if (slot == 0) {
        return 0;
    }
    if (!tg_gui_photo_pipeline_acquire(slot, TG_GUI_PHOTO_SCOPE_INLINE)) {
        return 0;
    }
    tg_gui_photo_request_remove_first();
    if (tg_gui_photo_canonical_load_start(
            slot, canonical_w, canonical_h, 0)) {
        return 1;
    }
    sprintf(path, "photos/tgph%08lx%08lx.jpg", request.id_hi, request.id_lo);
    f = fopen(path, "rb");
    if (f == 0 || fseek(f, 0L, SEEK_END) != 0) {
        if (f != 0) {
            fclose(f);
        }
        preview_ready = 0;
        if (slot->state == 0) {
            preview_ready = tg_gui_photo_preview_start(
                ctx, slot, request.source_w, request.source_h,
                TG_GUI_PHOTO_PREVIEW_CAP, 0);
            if (!preview_ready) {
                tg_gui_photo_slot_clear(slot);
            }
        }
        tg_gui_photo_diag("photo: decode wait cache missing");
        tg_gui_photo_pipeline_release(slot);
        return preview_ready;
    }
    flen = ftell(f);
    if (flen <= 0L || (unsigned long)flen > TG_GUI_PHOTO_JPEG_MAX ||
        fseek(f, 0L, SEEK_SET) != 0) {
        fclose(f);
        tg_gui_photo_diag("photo: decode fail cache size");
        tg_gui_photo_pipeline_release(slot);
        return 0;
    }
    slot->jpeg = (unsigned char *)malloc((size_t)flen);
    if (slot->jpeg == 0) {
        fclose(f);
        tg_gui_photo_diag("photo: decode wait (no memory)");
        tg_gui_photo_pipeline_release(slot);
        tg_gui_photo_request_offer(request.id_hi, request.id_lo,
                                   request.source_w, request.source_h);
        return 0;
    }
    got = (unsigned long)fread(slot->jpeg, 1, (size_t)flen, f);
    fclose(f);
    if (got != (unsigned long)flen) {
        tg_gui_photo_diag("photo: decode wait cache read");
        free(slot->jpeg);
        slot->jpeg = 0;
        tg_gui_photo_pipeline_release(slot);
        tg_gui_photo_request_offer(request.id_hi, request.id_lo,
                                   request.source_w, request.source_h);
        return 0;
    }
    slot->jpeg_len = got;
    slot->decode_w = canonical_w;
    slot->decode_h = canonical_h;
    slot->state = 2;
    if (!tg_gui_photo_begin_quality_sequence(
            slot, TG_GUI_PHOTO_DECODE_CAP, &decode_rc)) {
        tg_gui_photo_decode_reject(slot, decode_rc);
        return 0;
    }
    if (tg_gui_log_is_enabled()) {
        char line[80];

        sprintf(line, "photo: decode begin %dx%d at 1/%d",
                slot->decode_w, slot->decode_h, 1 << slot->pass_scale);
        tg_gui_log(line);
    }
    return 1;
}

/* One bounded idle slice. JPEG entropy, cache I/O and palette replay all stay
   outside paint/NEWSIZE and are reported as distinct work kinds. A return of
   1 means a complete quality pass became drawable (or a failure changed the
   placeholder state). */
static int tg_gui_photo_decode_tick(tg_gui_amiga_ctx *ctx,
                                    unsigned int mcu_budget,
                                    int pen_row_budget,
                                    unsigned long cache_budget,
                                    int *used_turn,
                                    int *work_kind)
{
    tg_gui_photo_slot *slot;
    tg_gui_photo_slot *offscreen_active;
    int i;
    int best_priority;
    int decode_rc;
    int changed;

    if (used_turn != 0) {
        *used_turn = 0;
    }
    if (work_kind != 0) {
        *work_kind = TG_GUI_PHOTO_WORK_NONE;
    }
    if (ctx == 0 || ctx->photo_resize_active || mcu_budget == 0U ||
        pen_row_budget <= 0) {
        return 0;
    }
    slot = tg_gui_photo_pipeline_owner(TG_GUI_PHOTO_SCOPE_INLINE);
    if (tg_gui_photo_decode_pipeline.owner != 0 && slot == 0) {
        /* The viewer owns the shared quality pipe. Inline requests stay queued
           until its complete frame has committed. */
        return 0;
    }
    if (slot != 0 && slot->state != 2) {
        tg_gui_photo_pipeline_release(slot);
        slot = 0;
    }
    if (slot == 0) {
        offscreen_active = 0;
        best_priority = TG_GUI_PHOTO_VISIBLE_MAX + 1;
        for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
            if (tg_gui_photo_slots[i].state == 2) {
                int priority;

                priority = tg_gui_photo_visible_priority(
                    tg_gui_photo_slots[i].id_hi,
                    tg_gui_photo_slots[i].id_lo);
                if (priority >= 0 && priority < best_priority) {
                    slot = &tg_gui_photo_slots[i];
                    best_priority = priority;
                } else if (priority < 0 &&
                           (offscreen_active == 0 ||
                            tg_gui_photo_slots[i].last_use <
                                offscreen_active->last_use)) {
                    offscreen_active = &tg_gui_photo_slots[i];
                }
            }
        }
        if (slot == 0) {
            slot = offscreen_active;
        }
        if (slot != 0 &&
            !tg_gui_photo_pipeline_acquire(slot, TG_GUI_PHOTO_SCOPE_INLINE)) {
            return 0;
        }
    }
    if (slot == 0 && tg_gui_photo_request_count > 0) {
        if (!tg_gui_photo_decode_start(ctx)) {
            return 0;
        }
        slot = tg_gui_photo_pipeline_owner(TG_GUI_PHOTO_SCOPE_INLINE);
        if (slot != 0) {
            if (used_turn != 0) {
                *used_turn = 1;
            }
            if (work_kind != 0) {
                *work_kind = TG_GUI_PHOTO_WORK_CACHE;
            }
        }
        /* File open/read and decoder setup are a cache/setup turn. Entropy
           work begins on the next local step and gets an uncontaminated time. */
        return 0;
    }
    if (slot == 0 || slot->state != 2 ||
        !tg_gui_photo_pipeline_owns(slot, TG_GUI_PHOTO_SCOPE_INLINE)) {
        return 0;
    }
    if (used_turn != 0) {
        *used_turn = 1;
    }
    changed = tg_gui_photo_quality_tick(
        ctx, slot, mcu_budget, pen_row_budget, cache_budget,
        TG_GUI_PHOTO_DECODE_CAP, 0, &decode_rc, work_kind);
    if (changed < 0) {
        tg_gui_photo_decode_reject(slot, decode_rc);
        return 1;
    }
    return changed;
}

static void tg_gui_photo_queue_pen_fallback(tg_gui_amiga_ctx *ctx)
{
    int i;

    if (ctx == 0) {
        return;
    }
    ctx->photo_truecolor = 0;
    ctx->photo_cgx_failed = 1;
    ctx->photo_direct_count = 0;
    if (ctx->photo_viewer_scope) {
        return;
    }
    for (i = 0; i < TG_GUI_PHOTO_SLOTS; ++i) {
        if (tg_gui_photo_slots[i].state > 0 &&
            tg_gui_photo_slots[i].rgb != 0 &&
            tg_gui_photo_slots[i].pen == 0) {
            tg_gui_photo_slots[i].render_logged = 0;
            tg_gui_photo_slots[i].state = 2;
        }
    }
}

/* A cybergraphics library can be present while the layerless friend bitmap is
   not a CyberGraphX bitmap. Track the buffer and the real window separately:
   MorphOS commonly rejects the friend bitmap while accepting the window RP. */
#if defined(TG_GUI_HAVE_CYBERGRAPHICS)
static int tg_gui_photo_cgx_target_possible(struct RastPort *rport)
{
    return rport != 0 && rport->BitMap != 0 && CyberGfxBase != 0 &&
           tg_gui_cgx_get_map_attr(
               rport->BitMap, CYBRMATTR_ISCYBERGFX) != 0UL &&
           tg_gui_cgx_get_map_attr(rport->BitMap, CYBRMATTR_DEPTH) > 8UL;
}

static int tg_gui_photo_cgx_self_check(tg_gui_amiga_ctx *ctx,
                                       struct RastPort *target,
                                       int origin_x, int origin_y,
                                       int x, int y)
{
    unsigned char expected[6] = { 255U, 0U, 0U, 0U, 255U, 0U };
    unsigned char actual[6];
    int is_window;
    int *checked;
    int *usable;
    int test_x;
    int test_y;

    if (ctx == 0 || ctx->photo_cgx_failed || !ctx->photo_truecolor ||
        target == 0 || target->BitMap == 0 || CyberGfxBase == 0) {
        return 0;
    }
    is_window = ctx->window != 0 && target == ctx->window->RPort;
    checked = is_window ? &ctx->photo_window_cgx_checked
                        : &ctx->photo_cgx_checked;
    usable = is_window ? &ctx->photo_window_cgx_usable
                       : &ctx->photo_cgx_usable;
    if (*checked) {
        return *usable;
    }
    *checked = 1;
    *usable = 0;
    if (!tg_gui_photo_cgx_target_possible(target) ||
        ctx->inner_w < 2 || ctx->inner_h < 1) {
        return 0;
    }
    test_x = x;
    test_y = y;
    if (test_x < 0) {
        test_x = 0;
    } else if (test_x + 1 >= ctx->inner_w) {
        test_x = ctx->inner_w - 2;
    }
    if (test_y < 0) {
        test_y = 0;
    } else if (test_y >= ctx->inner_h) {
        test_y = ctx->inner_h - 1;
    }
    memset(actual, 0, sizeof(actual));
    if (tg_gui_cgx_write_pixel_array(
            expected, 0, 0, 6, target,
            (UWORD)(origin_x + test_x), (UWORD)(origin_y + test_y),
            2, 1, RECTFMT_RGB) == 0UL ||
        tg_gui_cgx_read_pixel_array(
            actual, 0, 0, 6, target,
            (UWORD)(origin_x + test_x), (UWORD)(origin_y + test_y),
            2, 1, RECTFMT_RGB) == 0UL ||
        actual[0] < 200U || actual[1] > 55U || actual[2] > 55U ||
        actual[3] > 55U || actual[4] < 200U || actual[5] > 55U) {
        if (is_window) {
            ctx->photo_direct_report = -1;
        } else {
            tg_gui_photo_diag("photo: buffer cgx self-check failed");
        }
        return 0;
    }
    *usable = 1;
    if (is_window) {
        if (ctx->photo_direct_report == 0) {
            ctx->photo_direct_report = 1;
        }
    } else {
        tg_gui_photo_diag("photo: buffer cgx self-check passed");
    }
    return 1;
}
#endif

/* How many scaled rows are staged before handing them to cybergraphics in one
   call. One row per call cost 220-820 ms per slice on a 68k with P96/AfA (the
   library overhead dwarfed the pixel work, field report 2026-08-07): the
   photo crawled and the pacer, seeing slow slices, shrank its budget on top.
   A block of rows turns that into one call per block. */
#if defined(__m68k__)
#define TG_GUI_PHOTO_REPLAY_ROWS 8
#else
#define TG_GUI_PHOTO_REPLAY_ROWS 16
#endif

/* Replay canonical RGB888 through cybergraphics in blocks of scaled rows.
   Horizontal/vertical nearest scaling is CPU-cheap and never touches JPEG or
   the pen allocator. The target can be either the friend bitmap or the real
   window RastPort. */
static int tg_gui_photo_draw_truecolor_target(
    tg_gui_amiga_ctx *ctx, struct RastPort *target,
    int origin_x, int origin_y, const tg_gui_photo_slot *slot,
    tg_gui_rect rect, int x0, int y0, int x1, int y1)
{
#if defined(TG_GUI_HAVE_CYBERGRAPHICS)
    static unsigned char rows[TG_GUI_PHOTO_REPLAY_CAP * 3 *
                              TG_GUI_PHOTO_REPLAY_ROWS];
    int y;
    int chunk_x;

    if (!ctx->photo_truecolor || slot->rgb == 0 || CyberGfxBase == 0 ||
        !tg_gui_photo_cgx_self_check(ctx, target, origin_x, origin_y,
                                     x0, y0)) {
        return 0;
    }
    if (x1 <= x0) {
        return 0;
    }
    for (chunk_x = x0; chunk_x < x1; chunk_x += TG_GUI_PHOTO_REPLAY_CAP) {
        int width;

        width = x1 - chunk_x;
        if (width > TG_GUI_PHOTO_REPLAY_CAP) {
            width = TG_GUI_PHOTO_REPLAY_CAP;
        }
        for (y = y0; y < y1; y += TG_GUI_PHOTO_REPLAY_ROWS) {
            int block;
            int i;

            block = y1 - y;
            if (block > TG_GUI_PHOTO_REPLAY_ROWS) {
                block = TG_GUI_PHOTO_REPLAY_ROWS;
            }
            for (i = 0; i < block; ++i) {
                unsigned char *dst;
                int sy;
                int x;

                sy = (((y + i) - rect.y) * slot->h) / rect.h;
                dst = rows + ((unsigned long)i * (unsigned long)width * 3UL);
                for (x = 0; x < width; ++x) {
                    int sx;
                    const unsigned char *src;

                    sx = (((chunk_x + x) - rect.x) * slot->w) / rect.w;
                    src = slot->rgb +
                          (((unsigned long)sy * (unsigned long)slot->w +
                            (unsigned long)sx) * 3UL);
                    dst[x * 3] = src[0];
                    dst[x * 3 + 1] = src[1];
                    dst[x * 3 + 2] = src[2];
                }
                if (tg_gui_profile_active) {
                    ++tg_gui_profile_photo_rgb_rows;
                }
            }
            if (tg_gui_cgx_write_pixel_array(
                    rows, 0, 0, (UWORD)(width * 3), target,
                    (UWORD)(origin_x + chunk_x), (UWORD)(origin_y + y),
                    (UWORD)width, (UWORD)block, RECTFMT_RGB) == 0UL) {
                return 0;
            }
        }
    }
    return 1;
#else
    (void)ctx;
    (void)target;
    (void)origin_x;
    (void)origin_y;
    (void)slot;
    (void)rect;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    return 0;
#endif
}

static void tg_gui_photo_direct_begin(tg_gui_amiga_ctx *ctx)
{
    if (ctx != 0) {
        ctx->photo_direct_count = 0;
    }
}

static int tg_gui_photo_direct_queue(tg_gui_amiga_ctx *ctx,
                                     tg_gui_photo_slot *slot,
                                     tg_gui_rect rect,
                                     int x0, int y0, int x1, int y1)
{
#if defined(TG_GUI_HAVE_CYBERGRAPHICS)
    tg_gui_photo_direct_op *op;

    if (ctx == 0 || slot == 0 || ctx->window == 0 ||
        ctx->rport == ctx->window->RPort ||
        ctx->photo_direct_count >= TG_GUI_PHOTO_DIRECT_OPS ||
        !tg_gui_photo_cgx_target_possible(ctx->window->RPort)) {
        return 0;
    }
    op = &ctx->photo_direct_ops[ctx->photo_direct_count++];
    op->slot = slot;
    op->rect = rect;
    op->x0 = x0;
    op->y0 = y0;
    op->x1 = x1;
    op->y1 = y1;
    if (!ctx->photo_direct_logged) {
        tg_gui_photo_diag("photo: window cgx path");
        ctx->photo_direct_logged = 1;
    }
    return 1;
#else
    (void)ctx;
    (void)slot;
    (void)rect;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    return 0;
#endif
}

/* A full off-screen paint records the RGB photo rectangles when its friend
   bitmap is not CGX. Replay only the rectangles touched by the following
   window blit; the layer/BeginRefresh lock is owned by the caller. */
static int tg_gui_photo_direct_replay(tg_gui_amiga_ctx *ctx,
                                      int dirty_x, int dirty_y,
                                      int dirty_w, int dirty_h)
{
#if defined(TG_GUI_HAVE_CYBERGRAPHICS)
    int i;
    int any;

    if (ctx == 0 || ctx->window == 0 || ctx->photo_direct_count <= 0 ||
        dirty_w <= 0 || dirty_h <= 0) {
        return 1;
    }
    any = 0;
    for (i = 0; i < ctx->photo_direct_count; ++i) {
        tg_gui_photo_direct_op *op = &ctx->photo_direct_ops[i];

        if (op->x1 > dirty_x && op->x0 < dirty_x + dirty_w &&
            op->y1 > dirty_y && op->y0 < dirty_y + dirty_h) {
            any = 1;
            break;
        }
    }
    if (!any) {
        return 1;
    }
    /* BltBitMapRastPort can still own the destination after returning. */
    WaitBlit();
    for (i = 0; i < ctx->photo_direct_count; ++i) {
        tg_gui_photo_direct_op *op = &ctx->photo_direct_ops[i];
        tg_gui_photo_slot *slot = op->slot;

        if (op->x1 <= dirty_x || op->x0 >= dirty_x + dirty_w ||
            op->y1 <= dirty_y || op->y0 >= dirty_y + dirty_h) {
            continue;
        }
        if (slot == 0 || slot->rgb == 0 || slot->state <= 0 ||
            !tg_gui_photo_draw_truecolor_target(
                ctx, ctx->window->RPort, ctx->origin_x, ctx->origin_y,
                slot, op->rect, op->x0, op->y0, op->x1, op->y1)) {
            ctx->photo_direct_report = -1;
            tg_gui_photo_queue_pen_fallback(ctx);
            if (ctx->photo_viewer_scope && slot != 0 && slot->rgb != 0 &&
                slot->pen == 0) {
                slot->state = 2;
                slot->render_logged = 0;
            }
            return 0;
        }
    }
    return 1;
#else
    (void)ctx;
    (void)dirty_x;
    (void)dirty_y;
    (void)dirty_w;
    (void)dirty_h;
    return 1;
#endif
}

static void tg_gui_photo_direct_report(tg_gui_amiga_ctx *ctx)
{
    int report;

    if (ctx == 0) {
        return;
    }
    report = ctx->photo_direct_report;
    ctx->photo_direct_report = 0;
    if (report < 0) {
        tg_gui_photo_diag("photo: window cgx replay failed, pen fallback");
    } else if (report > 0) {
        tg_gui_photo_diag("photo: window cgx self-check passed");
    }
}

static int tg_gui_photo_draw_truecolor(tg_gui_amiga_ctx *ctx,
                                       tg_gui_photo_slot *slot,
                                       tg_gui_rect rect,
                                       int x0, int y0, int x1, int y1)
{
    if (tg_gui_photo_draw_truecolor_target(
            ctx, ctx->rport, ctx->origin_x, ctx->origin_y,
            slot, rect, x0, y0, x1, y1)) {
        return 1;
    }
    return tg_gui_photo_direct_queue(ctx, slot, rect, x0, y0, x1, y1);
}

/* Replay one complete canonical quality pass into a clipped rectangle. The
   transcript and popup viewer share this path and runtime CGX fallback, but
   keep separate slot ownership. Hidden decode buffers never reach paint. */
static int tg_gui_photo_draw_slot(tg_gui_amiga_ctx *ctx,
                                  tg_gui_photo_slot *slot,
                                  tg_gui_rect rect,
                                  tg_gui_rect clip)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int y;
    int display_ready;

    if (ctx == 0 || slot == 0 || rect.w <= 0 || rect.h <= 0 ||
        tg_gui_av_cmap == 0 || ctx->photo_resize_active || slot->state < 0 ||
        slot->w <= 0 || slot->h <= 0) {
        return 0;
    }
    display_ready = ctx->photo_truecolor
        ? slot->rgb != 0 && slot->ready_rows >= slot->h
        : slot->pen != 0 && slot->pen_rows >= slot->h;
    if (!display_ready) {
        return 0;
    }
    x0 = rect.x > clip.x ? rect.x : clip.x;
    y0 = rect.y > clip.y ? rect.y : clip.y;
    x1 = rect.x + rect.w;
    if (x1 > clip.x + clip.w) {
        x1 = clip.x + clip.w;
    }
    y1 = rect.y + rect.h;
    if (y1 > clip.y + clip.h) {
        y1 = clip.y + clip.h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    tg_gui_prim_log("pimg", x0, y0, x1 - x0, y1 - y0);
    if (ctx->photo_truecolor && slot->rgb != 0) {
        if (!slot->render_logged) {
            tg_gui_photo_diag("photo: cgx path");
            slot->render_logged = 1;
        }
        if (tg_gui_photo_draw_truecolor(ctx, slot, rect, x0, y0, x1, y1)) {
            return 1;
        }
        if (!ctx->photo_cgx_failed) {
            /* A later driver failure gets the same permanent portable path as
               a failed self-check. Never quantize inside paint. */
            if (ctx->window != 0 && ctx->rport == ctx->window->RPort) {
                ctx->photo_direct_report = -1;
            } else {
                tg_gui_photo_diag("photo: cgx replay failed, pen fallback");
            }
            tg_gui_photo_queue_pen_fallback(ctx);
        }
        /* The viewer slot is outside the transcript cache array. Queue its own
           palette mapping after a failed RGB self-check/replay. */
        if (!ctx->photo_truecolor && slot->rgb != 0 && slot->pen == 0) {
            slot->state = 2;
            slot->render_logged = 0;
        }
        return 0;
    }
    if (slot->pen == 0) {
        return 0;
    }
    if (!slot->render_logged) {
        tg_gui_photo_diag("photo: pen path");
        slot->render_logged = 1;
    }
    for (y = y0; y < y1; ++y) {
        int sy;
        int x;

        sy = ((y - rect.y) * slot->h) / rect.h;
        x = x0;
        while (x < x1) {
            int sx;
            unsigned char p;
            int run;

            sx = ((x - rect.x) * slot->w) / rect.w;
            p = slot->pen[sy * slot->w + sx];
            run = x + 1;
            while (run < x1 &&
                   slot->pen[sy * slot->w +
                             (((run - rect.x) * slot->w) / rect.w)] == p) {
                ++run;
            }
            SetAPen(ctx->rport, (LONG)p);
            RectFill(ctx->rport, ctx->origin_x + x, ctx->origin_y + y,
                     ctx->origin_x + run - 1, ctx->origin_y + y);
            if (tg_gui_profile_active) {
                ++tg_gui_profile_photo_pen_runs;
            }
            x = run;
        }
    }
    return 1;
}

static int tg_gui_amiga_photo_image(tg_gui_backend *backend,
                                    unsigned long id_hi,
                                    unsigned long id_lo,
                                    unsigned long source_w,
                                    unsigned long source_h,
                                    tg_gui_rect rect,
                                    tg_gui_rect clip)
{
    tg_gui_amiga_ctx *ctx;
    tg_gui_photo_slot *slot;
    int cache_state;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    if (rect.w <= 0 || rect.h <= 0 || (id_hi == 0UL && id_lo == 0UL) ||
        tg_gui_av_cmap == 0 || ctx->photo_resize_active) {
        return 0;
    }
    (void)tg_gui_photo_visible_mark(id_hi, id_lo);
    slot = tg_gui_photo_slot_find(id_hi, id_lo);
    cache_state = tg_gui_session_request_inline_photo(id_hi, id_lo);
    if (slot == 0) {
        /* Paint is I/O-free: it only queues visible cache entries. INTUITICKS
           advance hidden quality passes and publish only complete frames. */
        if (cache_state != 0) {
            tg_gui_photo_request_offer(id_hi, id_lo, source_w, source_h);
        }
        return 0;
    }
    if (slot->preview_only && slot->state == 1 && cache_state == 2) {
        tg_gui_photo_request_offer(id_hi, id_lo, source_w, source_h);
    }
    tg_gui_photo_slot_touch(slot);
    return tg_gui_photo_draw_slot(ctx, slot, rect, clip);
}

/* Render bitmap-font text without AfA's layerless Text() path. The old
   workaround issued one BltTemplate per glyph (and per row for italic), which
   made a full RTG repaint disproportionately expensive. Compose a whole run in
   a reusable 1-bit template, then submit it in one blit. */
#define TG_GUI_AFA_TEMPLATE_BYTES 16384U
#define TG_GUI_AFA_RUN_CHARS 512UL
static UWORD tg_gui_afa_template[TG_GUI_AFA_TEMPLATE_BYTES / sizeof(UWORD)];
static unsigned long tg_gui_afa_template_blits;
static unsigned long tg_gui_afa_template_chars;

static int tg_gui_amiga_char_advance(const struct TextFont *font,
                                     const struct RastPort *rp,
                                     unsigned long index)
{
    int advance;

    if (font->tf_CharSpace != 0) {
        advance = (int)((WORD *)font->tf_CharSpace)[index];
    } else {
        advance = (int)font->tf_XSize;
    }
    return advance + (int)rp->TxSpacing;
}

static void tg_gui_amiga_italic_bounds(const struct TextFont *font,
                                       ULONG style,
                                       int *out_min, int *out_max)
{
    int row;
    int check;
    int shift;
    int min_shift;
    int max_shift;

    min_shift = max_shift = 0;
    if ((style & FSF_ITALIC) != 0) {
        check = (int)font->tf_Baseline;
        shift = check / 2;
        min_shift = max_shift = shift;
        for (row = 0; row < (int)font->tf_YSize; ++row) {
            if (shift < min_shift) {
                min_shift = shift;
            }
            if (shift > max_shift) {
                max_shift = shift;
            }
            --check;
            if ((check & 1) != 0) {
                --shift;
            }
        }
    }
    *out_min = min_shift;
    *out_max = max_shift;
}

static int tg_gui_amiga_blt_text_run(struct RastPort *rp, int x,
                                     int baseline, const char *text,
                                     unsigned long length,
                                     int *out_advance)
{
    struct TextFont *font;
    unsigned long i;
    int cursor;
    int min_x;
    int max_x;
    int italic_min;
    int italic_max;
    int bold_max;
    int width;
    int row_bytes;
    unsigned long template_bytes;
    UBYTE *template_data;

    font = rp->Font;
    if (font == 0 || font->tf_CharData == 0 || font->tf_CharLoc == 0 ||
        font->tf_YSize == 0 || length == 0UL) {
        return 0;
    }
    tg_gui_amiga_italic_bounds(font, rp->AlgoStyle,
                               &italic_min, &italic_max);
    bold_max = ((rp->AlgoStyle & FSF_BOLD) != 0)
                   ? (int)font->tf_BoldSmear : 0;
    cursor = 0;
    min_x = 0;
    max_x = 0;
    for (i = 0UL; i < length; ++i) {
        unsigned long index;
        ULONG charloc;
        int glyph_x;
        int glyph_width;
        int left;
        int right;

        index = tg_gui_amiga_font_char_index(
            font, (unsigned int)(unsigned char)text[i]);
        charloc = ((ULONG *)font->tf_CharLoc)[index];
        glyph_width = (int)(UWORD)(charloc & 0xffffUL);
        glyph_x = cursor;
        if (font->tf_CharKern != 0) {
            glyph_x += (int)((WORD *)font->tf_CharKern)[index];
        }
        left = glyph_x + italic_min;
        right = glyph_x + glyph_width + italic_max + bold_max;
        if (left < min_x) {
            min_x = left;
        }
        if (right > max_x) {
            max_x = right;
        }
        cursor += tg_gui_amiga_char_advance(font, rp, index);
        if (cursor > max_x) {
            max_x = cursor;
        }
    }
    width = max_x - min_x;
    row_bytes = ((width + 15) / 16) * 2;
    template_bytes = (unsigned long)row_bytes * (unsigned long)font->tf_YSize;
    if (width <= 0 || row_bytes <= 0 ||
        template_bytes > TG_GUI_AFA_TEMPLATE_BYTES) {
        return 0;
    }
    template_data = (UBYTE *)tg_gui_afa_template;
    memset(template_data, 0, (size_t)template_bytes);
    cursor = 0;
    for (i = 0UL; i < length; ++i) {
        unsigned long index;
        ULONG charloc;
        UWORD glyph_pos;
        UWORD glyph_width;
        int glyph_x;
        int row;
        int italic_check;
        int italic_shift;

        index = tg_gui_amiga_font_char_index(
            font, (unsigned int)(unsigned char)text[i]);
        charloc = ((ULONG *)font->tf_CharLoc)[index];
        glyph_pos = (UWORD)(charloc >> 16);
        glyph_width = (UWORD)(charloc & 0xffffUL);
        glyph_x = cursor;
        if (font->tf_CharKern != 0) {
            glyph_x += (int)((WORD *)font->tf_CharKern)[index];
        }
        italic_check = (int)font->tf_Baseline;
        italic_shift = ((rp->AlgoStyle & FSF_ITALIC) != 0)
                           ? italic_check / 2 : 0;
        for (row = 0; row < (int)font->tf_YSize; ++row) {
            UBYTE *dst_row;
            const UBYTE *src_row;
            int col;

            dst_row = template_data + (unsigned long)row * row_bytes;
            src_row = (const UBYTE *)font->tf_CharData +
                      (unsigned long)row * font->tf_Modulo;
            for (col = 0; col < (int)glyph_width; ++col) {
                unsigned int source_bit;

                source_bit = (unsigned int)glyph_pos + (unsigned int)col;
                if ((src_row[source_bit >> 3] &
                     (UBYTE)(0x80U >> (source_bit & 7U))) != 0) {
                    int passes;
                    int pass;

                    passes = ((rp->AlgoStyle & FSF_BOLD) != 0) ? 2 : 1;
                    for (pass = 0; pass < passes; ++pass) {
                        int dx;

                        dx = glyph_x + col + italic_shift - min_x;
                        if (pass != 0) {
                            dx += (int)font->tf_BoldSmear;
                        }
                        if (dx >= 0 && dx < width) {
                            dst_row[dx >> 3] |=
                                (UBYTE)(0x80U >> ((unsigned int)dx & 7U));
                        }
                    }
                }
            }
            if ((rp->AlgoStyle & FSF_ITALIC) != 0) {
                --italic_check;
                if ((italic_check & 1) != 0) {
                    --italic_shift;
                }
            }
        }
        cursor += tg_gui_amiga_char_advance(font, rp, index);
    }
    BltTemplate((PLANEPTR)template_data, 0L, (LONG)row_bytes, rp,
                (LONG)(x + min_x),
                (LONG)(baseline - (int)font->tf_Baseline),
                (LONG)width, (LONG)font->tf_YSize);
    ++tg_gui_afa_template_blits;
    tg_gui_afa_template_chars += length;
    *out_advance = cursor;
    return 1;
}

/* A single-glyph fallback for an unusually large font/run that cannot fit the
   bounded scratch template. Normal UI fonts never take this path. */
static int tg_gui_amiga_blt_text_glyph(struct RastPort *rp, int x,
                                       int baseline, unsigned char c)
{
    struct TextFont *font;
    unsigned long index;
    ULONG charloc;
    UWORD glyph_pos;
    UWORD glyph_width;
    int glyph_x;
    int bold_pass;
    int bold_passes;

    font = rp->Font;
    index = tg_gui_amiga_font_char_index(font, (unsigned int)c);
    charloc = ((ULONG *)font->tf_CharLoc)[index];
    glyph_pos = (UWORD)(charloc >> 16);
    glyph_width = (UWORD)(charloc & 0xffffUL);
    glyph_x = x;
    if (font->tf_CharKern != 0) {
        glyph_x += (int)((WORD *)font->tf_CharKern)[index];
    }
    bold_passes = ((rp->AlgoStyle & FSF_BOLD) != 0) ? 2 : 1;
    for (bold_pass = 0; bold_pass < bold_passes; ++bold_pass) {
        int bold_x;

        bold_x = bold_pass ? (int)font->tf_BoldSmear : 0;
        if ((rp->AlgoStyle & FSF_ITALIC) != 0) {
            int row;
            int italic_check;
            int italic_shift;

            italic_check = (int)font->tf_Baseline;
            italic_shift = italic_check / 2;
            for (row = 0; row < (int)font->tf_YSize; ++row) {
                BltTemplate(
                    (PLANEPTR)((UBYTE *)font->tf_CharData +
                               ((unsigned long)row * font->tf_Modulo) +
                               (glyph_pos >> 3)),
                    (LONG)(glyph_pos & 7U), (LONG)font->tf_Modulo, rp,
                    (LONG)(glyph_x + bold_x + italic_shift),
                    (LONG)(baseline - (int)font->tf_Baseline + row),
                    (LONG)glyph_width, 1L);
                ++tg_gui_afa_template_blits;
                if (tg_gui_profile_active) {
                    ++tg_gui_profile_afa_fallback_blits;
                }
                --italic_check;
                if ((italic_check & 1) != 0) {
                    --italic_shift;
                }
            }
        } else {
            BltTemplate((PLANEPTR)((UBYTE *)font->tf_CharData +
                                   (glyph_pos >> 3)),
                        (LONG)(glyph_pos & 7U), (LONG)font->tf_Modulo, rp,
                        (LONG)(glyph_x + bold_x),
                        (LONG)(baseline - (int)font->tf_Baseline),
                        (LONG)glyph_width, (LONG)font->tf_YSize);
            ++tg_gui_afa_template_blits;
            if (tg_gui_profile_active) {
                ++tg_gui_profile_afa_fallback_blits;
            }
        }
    }
    ++tg_gui_afa_template_chars;
    return tg_gui_amiga_char_advance(font, rp, index);
}

static void tg_gui_amiga_blt_text(struct RastPort *rp, int x, int baseline,
                                  const char *text, unsigned long length)
{
    unsigned long pos;
    int cursor;

    if (rp == 0 || text == 0 || length == 0UL || rp->Font == 0 ||
        rp->Font->tf_CharData == 0 || rp->Font->tf_CharLoc == 0) {
        return;
    }
    pos = 0UL;
    cursor = x;
    while (pos < length) {
        unsigned long chunk;
        int advance;

        chunk = length - pos;
        if (chunk > TG_GUI_AFA_RUN_CHARS) {
            chunk = TG_GUI_AFA_RUN_CHARS;
        }
        advance = 0;
        while (chunk > 1UL &&
               !tg_gui_amiga_blt_text_run(rp, cursor, baseline,
                                           text + pos, chunk, &advance)) {
            chunk /= 2UL;
        }
        if (chunk == 1UL &&
            !tg_gui_amiga_blt_text_run(rp, cursor, baseline,
                                        text + pos, 1UL, &advance)) {
            advance = tg_gui_amiga_blt_text_glyph(
                rp, cursor, baseline, (unsigned char)text[pos]);
        }
        cursor += advance;
        pos += chunk;
    }
    if ((rp->AlgoStyle & FSF_UNDERLINED) != 0 && cursor > x) {
        RectFill(rp, (LONG)x, (LONG)(baseline + 1), (LONG)(cursor - 1),
                 (LONG)(baseline + 1));
    }
    Move(rp, (LONG)cursor, (LONG)baseline);
}

/* One plain run of text at (x, baseline). */
static void tg_gui_amiga_draw_run(tg_gui_amiga_ctx *ctx, int pen, int x,
                                  int baseline, const char *text,
                                  unsigned long length)
{
    if (length == 0UL) {
        return;
    }
    if (length > 0x7fffUL) {
        length = 0x7fffUL; /* Text count is 16-bit; clamp defensively */
    }
    /* Trail: position and LENGTH only -- chat content never reaches the log. */
    tg_gui_prim_log("text", x, baseline, (int)length, 0);
    SetAPen(ctx->rport, tg_gui_amiga_resolve_pen(ctx, pen));
    SetDrMd(ctx->rport, JAM1);
    Move(ctx->rport, ctx->origin_x + x, ctx->origin_y + baseline);
    if (ctx->bitmap_text_compat && ctx->rport == &ctx->buf_rp) {
        tg_gui_amiga_blt_text(ctx->rport, ctx->origin_x + x,
                              ctx->origin_y + baseline, text, length);
    } else {
        Text(ctx->rport, (STRPTR)text, (UWORD)length);
    }
}

/* Text with emoji pairs: plain runs go through the font, each pair is a
   glyph cell of the font height whose bottom sits on the descender line,
   so a face lines up with the letters beside it. */
static void tg_gui_amiga_draw_text(tg_gui_backend *backend, int pen, int x,
                                   int baseline, const char *text,
                                   unsigned long length)
{
    tg_gui_amiga_ctx *ctx = (tg_gui_amiga_ctx *)backend->context;
    unsigned long i = 0UL;
    unsigned long run_start = 0UL;
    unsigned long index;
    int cell = 0;

    while (i < length) {
        if (tg_gui_emoji_pair_at(text, length, i, &index)) {
            int ascent;

            if (cell == 0) {
                cell = tg_gui_amiga_emoji_cell(ctx);
            }
            tg_gui_amiga_draw_run(ctx, pen, x, baseline, text + run_start,
                                  i - run_start);
            x += tg_gui_amiga_run_width(ctx, text + run_start, i - run_start);
            if (cell >= TG_GUI_EMOJI_INLINE_MIN) {
                ascent = ctx->rport != 0 && ctx->rport->Font != 0
                             ? (int)ctx->rport->Font->tf_Baseline : cell - 2;
                tg_gui_amiga_glyph_image(backend, index, x,
                                         baseline - ascent - (cell - ascent - 1) / 2,
                                         cell);
                x += cell;
            } else {
                const char *t = tg_gui_session_emoji_text(index);
                unsigned long tl = (unsigned long)strlen(t);

                tg_gui_amiga_draw_run(ctx, pen, x, baseline, t, tl);
                x += tg_gui_amiga_run_width(ctx, t, tl);
            }
            i += 2UL;
            run_start = i;
        } else {
            ++i;
        }
    }
    tg_gui_amiga_draw_run(ctx, pen, x, baseline, text + run_start,
                          i - run_start);
}

/* Map the renderer's style bitmask to graphics.library soft styles. Bold and
   italic are the algorithmic font styles; code and strike reuse underline
   (no soft strikethrough exists). The OS clamps to what the font supports. */
static void tg_gui_amiga_set_style(tg_gui_backend *backend, int style)
{
    tg_gui_amiga_ctx *ctx;
    ULONG soft;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    soft = 0UL;
    if ((style & TG_GUI_STYLE_BOLD) != 0) {
        soft |= FSF_BOLD;
    }
    if ((style & TG_GUI_STYLE_ITALIC) != 0) {
        soft |= FSF_ITALIC;
    }
    if ((style & (TG_GUI_STYLE_CODE | TG_GUI_STYLE_STRIKE |
                  TG_GUI_STYLE_UNDERLINE)) != 0) {
        soft |= FSF_UNDERLINED;
    }
    SetSoftStyle(ctx->rport, soft, FSF_BOLD | FSF_ITALIC | FSF_UNDERLINED);
}

static ULONG tg_gui_amiga_rgb32(unsigned char component)
{
    ULONG c;

    c = (ULONG)component;
    return (c << 24) | (c << 16) | (c << 8) | c;
}

static void tg_gui_amiga_obtain_pens(tg_gui_amiga_ctx *ctx,
                                     struct ColorMap *cmap)
{
    struct DrawInfo *dri;
    int i;

    ctx->pens_held = 0;
    /* The MENU_* pens track the SCREEN's own menu colours (same DrawInfo the
       new-look menubar uses), so the context popup matches the user's theme
       on every lane -- OS4.1 dark menus stay dark, classic grey stays grey.
       The pens belong to the system: obtained stays -1, never ReleasePen'd.
       No DrawInfo (or a missing pen) falls back to the table RGB below. */
    dri = GetScreenDrawInfo(ctx->window->WScreen);
    for (i = 0; i < TG_GUI_PEN_COUNT; ++i) {
        if (dri != 0 && i >= TG_GUI_PEN_MENU_BACK &&
            i <= TG_GUI_PEN_MENU_FRAME && /* MENU pens only: PEN_LINK sits
                                             above them and must keep its
                                             own blue, not the system
                                             shadow the fallback picked */
            (int)dri->dri_NumPens > BACKGROUNDPEN) { /* highest index used */
            UWORD *p = dri->dri_Pens;

            ctx->pens_obtained[i] = -1;
            ctx->pens[i] =
                (i == TG_GUI_PEN_MENU_BACK)     ? (LONG)p[BACKGROUNDPEN]
                : (i == TG_GUI_PEN_MENU_TEXT)   ? (LONG)p[TEXTPEN]
                : (i == TG_GUI_PEN_MENU_FILL)   ? (LONG)p[FILLPEN]
                : (i == TG_GUI_PEN_MENU_FILLTEXT) ? (LONG)p[FILLTEXTPEN]
                                                  : (LONG)p[SHADOWPEN];
            continue;
        }
        /* Keep the raw result so release frees ONLY what was really obtained;
           the drawing value falls back to a stock pen when obtain fails, but
           that fallback must never be passed to ReleasePen. */
        ctx->pens_obtained[i] =
            ObtainBestPenA(cmap, tg_gui_amiga_rgb32(tg_gui_dark_pens[i].r),
                           tg_gui_amiga_rgb32(tg_gui_dark_pens[i].g),
                           tg_gui_amiga_rgb32(tg_gui_dark_pens[i].b), 0);
        ctx->pens[i] = (ctx->pens_obtained[i] == -1)
                           ? ((i == TG_GUI_PEN_WINDOW) ? 0L : 1L)
                           : ctx->pens_obtained[i];
    }
    if (dri != 0) {
        FreeScreenDrawInfo(ctx->window->WScreen, dri); /* pen NUMBERS copied */
    }
    for (i = 0; i < TG_GUI_AVATAR_COLORS; ++i) {
        ctx->avatar_obtained[i] =
            ObtainBestPenA(cmap, tg_gui_amiga_rgb32(tg_gui_avatar_rgb[i].r),
                           tg_gui_amiga_rgb32(tg_gui_avatar_rgb[i].g),
                           tg_gui_amiga_rgb32(tg_gui_avatar_rgb[i].b), 0);
        ctx->avatar_pens[i] =
            (ctx->avatar_obtained[i] == -1) ? 1L : ctx->avatar_obtained[i];
    }
    ctx->pens_held = 1;
}

static void tg_gui_amiga_release_pens(tg_gui_amiga_ctx *ctx,
                                      struct ColorMap *cmap)
{
    int i;

    if (!ctx->pens_held) {
        return;
    }
    for (i = 0; i < TG_GUI_PEN_COUNT; ++i) {
        if (ctx->pens_obtained[i] != -1) {
            ReleasePen(cmap, ctx->pens_obtained[i]);
        }
    }
    for (i = 0; i < TG_GUI_AVATAR_COLORS; ++i) {
        if (ctx->avatar_obtained[i] != -1) {
            ReleasePen(cmap, ctx->avatar_obtained[i]);
        }
    }
    tg_gui_av_release_pool(cmap); /* the shared real-avatar pen pool */
    tg_gui_amiga_blend_release(cmap); /* the round-chrome edge blends */
    ctx->pens_held = 0;
}

static void tg_gui_amiga_measure_geometry(tg_gui_amiga_ctx *ctx)
{
    struct Window *w;

    w = ctx->window;
    ctx->origin_x = w->BorderLeft;
    ctx->origin_y = w->BorderTop;
    ctx->inner_w = (int)w->Width - w->BorderLeft - w->BorderRight;
    ctx->inner_h = (int)w->Height - w->BorderTop - w->BorderBottom;
    if (ctx->inner_w < 1) {
        ctx->inner_w = 1;
    }
    if (ctx->inner_h < 1) {
        ctx->inner_h = 1;
    }
}

/* Mirrors the selected chat's name into the header title so the title line
   tracks the highlighted sidebar row as the user navigates. */
static void tg_gui_window_apply_selection(tg_gui_state *state)
{
    const char *name;
    unsigned long i;

    if (state == 0 || state->chat_count <= 0) {
        return;
    }
    if (state->selected_chat < 0) {
        state->selected_chat = 0;
    }
    if (state->selected_chat >= state->chat_count) {
        state->selected_chat = state->chat_count - 1;
    }
    name = state->chats[state->selected_chat].name;
    for (i = 0UL; i + 1UL < (unsigned long)sizeof(state->title) &&
                  name[i] != '\0'; ++i) {
        state->title[i] = name[i];
    }
    state->title[i] = '\0';
}

/* Serialize every direct render against Intuition's layer machinery.
   OpenWindowTagList guarantees only a non-NULL Window -- NOT when the layer is
   safe to draw -- and the input.device/intuition task edits this window's
   ClipRect list at activation, sizing and dragging. An unserialized RastPort
   write while it does so corrupts the cliprect chain and, on MorphOS, freezes
   the whole box inside layers3d (DSI). LockLayerRom()/UnlockLayerRom() (the only
   layers.library pair the autodocs sanction for Intuition windows; reached here
   through graphics.library / GfxBase, already open, so no extra library) blocks
   Intuition from touching the layer while we render. The bracket is short (one
   full renderer paint of pure graphics ops -- no Intuition calls inside, which
   the LockLayer autodoc forbids) and rport->Layer is the window's layer for our
   non-GIMMEZEROZERO window. The IDCMP_REFRESHWINDOW path does NOT use these: it
   already runs inside BeginRefresh()'s own layer lock. */
/* Free the off-screen double-buffer if present. Safe when buf_bm==0. */
static void tg_gui_amiga_buffer_free(tg_gui_amiga_ctx *ctx)
{
    if (ctx->buf_bm != 0) {
        /* BltBitMapRastPort() may return while the source bitmap is still in
           use. NEWSIZE reaches here soon after the last frame; freeing that
           source early corrupts RTG/blitter state and can freeze the machine. */
        WaitBlit();
        FreeBitMap(ctx->buf_bm);
        ctx->buf_bm = 0;
    }
    ctx->buf_ok = 0;
    ctx->buf_w = 0;
    ctx->buf_h = 0;
}

/* AfA_OS performs opaque resize by stretching the window's current pixels
   before it asks the application for intermediate refreshes. Blank the client
   area once at the first NEWSIZE so it stretches only a stable background,
   not stale chat rows. Native systems do not need the extra fill. */
static void tg_gui_amiga_resize_blank(tg_gui_amiga_ctx *ctx)
{
    struct Layer *layer;

    if (ctx == 0 || ctx->rport == 0 || ctx->window == 0 ||
        ctx->inner_w <= 0 || ctx->inner_h <= 0) {
        return;
    }
    tg_gui_log("resize: blank begin");
    WaitBlit();
    layer = ctx->rport->Layer;
    if (layer != 0) {
        LockLayerRom(layer);
    }
    SetAPen(ctx->rport, ctx->pens[TG_GUI_PEN_WINDOW]);
    RectFill(ctx->rport, ctx->origin_x, ctx->origin_y,
             ctx->origin_x + ctx->inner_w - 1,
             ctx->origin_y + ctx->inner_h - 1);
    if (layer != 0) {
        UnlockLayerRom(layer);
    }
    WaitBlit();
    tg_gui_log("resize: blank done");
}

/* (Re)allocate the off-screen buffer to the CURRENT inner_w/inner_h as a friend
   of the window bitmap, so depth/format/placement match (chunky on a gfx card,
   planar on AGA) and the blit is native. Frees any old buffer first. On any
   failure leaves buf_ok==0 so the paint path falls back to direct rendering.
   MUST run after tg_gui_amiga_measure_geometry() so the geometry is current. */
static void tg_gui_amiga_buffer_alloc(tg_gui_amiga_ctx *ctx)
{
    struct BitMap *src;
    struct BitMap *bm;
    int w;
    int h;
    unsigned long depth;

    tg_gui_amiga_buffer_free(ctx);
    if (ctx->window == 0 || ctx->rport == 0 || ctx->rport->BitMap == 0) {
        return;
    }
    src = ctx->rport->BitMap;
    w = ctx->inner_w;
    h = ctx->inner_h;
    if (w < 8 || h < 8) {
        return; /* below the window minimum: skip buffering */
    }
    depth = (unsigned long)GetBitMapAttr(src, BMA_DEPTH);
    bm = AllocBitMap((ULONG)w, (ULONG)h, (ULONG)depth, 0UL, src);
    if (bm == 0) {
        tg_gui_log("window: double-buffer alloc failed, direct render");
        return;
    }
    {
        /* Sized + depth line for the field log: an RTG setup that dies in the
           first paint tells us here what pixel format it was running. */
        char line[64];

        sprintf(line, "window: double-buffer %dx%d depth %lu", w, h, depth);
        tg_gui_log(line);
    }
    InitRastPort(&ctx->buf_rp);
    ctx->buf_rp.BitMap = bm;
    /* InitRastPort does NOT inherit a font; text_width()/draw_text() read it from
       ctx->rport (== &buf_rp during the off-screen pass), so set it now. */
    SetFont(&ctx->buf_rp, ctx->rport->Font);
    if (ctx->buf_rp.Font == 0) {
        FreeBitMap(bm);
        tg_gui_log("window: double-buffer has no font, direct render");
        return;
    }
    ctx->buf_bm = bm;
    ctx->buf_w = w;
    ctx->buf_h = h;
    ctx->buf_ok = 1;
    if (!ctx->photo_cgx_failed) {
        ctx->photo_cgx_checked = 0;
        ctx->photo_cgx_usable = 0;
    }
}

/* Full-window paint. With the off-screen buffer, render the whole frame INTO it
   (no layer, no lock), then copy it to the window in ONE BltBitMapRastPort under
   the same LockLayerRom discipline the direct path used -- the window only ever
   shows complete frames, so the clear-then-draw flicker is gone. Falls back to
   the direct render when no buffer is available (alloc failed / window too
   small). */
static void tg_gui_window_paint(const tg_gui_state *state,
                                tg_gui_backend *backend)
{
    tg_gui_amiga_ctx *c = (tg_gui_amiga_ctx *)backend->context;
    struct Layer *layer;
    /* One-shot trail (--gui-live-debug) around the FIRST full paint: an AmiKit
       12/13 field setup dies between "setup done" and "opened", and this names
       the killer half -- the off-screen render (no lock held) or the blit.
       tg_gui_log does DOS I/O, so the blit probes stay OUTSIDE LockLayerRom. */
    static int first_logged;

    if (c == 0 || c->rport == 0) {
        return;
    }
    tg_gui_photo_frame_begin();
    tg_gui_photo_direct_begin(c);
    layer = c->rport->Layer;
    if (c->buf_ok && c->buf_bm != 0 &&
        c->buf_w == c->inner_w && c->buf_h == c->inner_h) {
        struct RastPort *saved_rport = c->rport;
        int saved_ox = c->origin_x;
        int saved_oy = c->origin_y;
        int profile_paint;
        clock_t profile_start;
        clock_t profile_render_done;
        clock_t profile_blit_done;

        profile_paint = c->bitmap_text_compat && tg_gui_log_is_enabled() &&
                        c->afa_profile_paints < 24U;
        profile_start = (clock_t)-1;
        profile_render_done = (clock_t)-1;
        profile_blit_done = (clock_t)-1;

        /* The previous frame copy may still be reading this bitmap. Do not
           modify its pixels until the blitter has finished with the source. */
        WaitBlit();
        if (profile_paint) {
            tg_gui_profile_prims = 0UL;
            tg_gui_profile_photo_rgb_rows = 0UL;
            tg_gui_profile_photo_pen_runs = 0UL;
            tg_gui_profile_afa_fallback_blits = 0UL;
            tg_gui_afa_template_blits = 0UL;
            tg_gui_afa_template_chars = 0UL;
            profile_start = clock();
            tg_gui_profile_active = 1;
        }
        if (!first_logged) {
            tg_gui_log("paint1: off-screen render start");
            /* First buffered render only: one line per primitive, so a crash
               inside a graphics call names it. No lock is held here. */
            tg_gui_prim_trail = 1;
            if (c->bitmap_text_compat) {
                tg_gui_afa_template_blits = 0UL;
                tg_gui_afa_template_chars = 0UL;
            }
        }
        c->rport = &c->buf_rp;
        c->origin_x = 0;
        c->origin_y = 0;
        tg_gui_paint(state, backend);
        if (profile_paint) {
            tg_gui_profile_active = 0;
            profile_render_done = clock();
        }
        tg_gui_prim_trail = 0;
        c->rport = saved_rport;
        c->origin_x = saved_ox;
        c->origin_y = saved_oy;

        if (!first_logged && c->bitmap_text_compat &&
            tg_gui_log_is_enabled()) {
            char line[96];

            sprintf(line, "paint1: afa templates %lu for %lu chars",
                    tg_gui_afa_template_blits, tg_gui_afa_template_chars);
            tg_gui_log(line);
        }

        if (!first_logged) {
            tg_gui_log("paint1: blit start");
        }
        if (layer != 0) {
            LockLayerRom(layer);
        }
        BltBitMapRastPort(c->buf_bm, 0, 0, c->rport, saved_ox, saved_oy,
                          c->inner_w, c->inner_h, 0xC0);
        (void)tg_gui_photo_direct_replay(c, 0, 0,
                                         c->inner_w, c->inner_h);
        /* The replay wrote photos straight into the window, on top of any
           popup the buffer had composed last (field report on MorphOS: the
           context menu, and now the emoji panel, behind a picture). Paint
           the popups once more, directly on the window this time; the
           painters use the same graphics calls the replay just did. */
        if (state->ctx_visible || state->mention_active ||
            state->emoji_active) {
            tg_gui_paint_popups(state, backend);
        }
        if (layer != 0) {
            UnlockLayerRom(layer);
        }
        tg_gui_photo_direct_report(c);
        if (profile_paint) {
            char line[160];
            unsigned long render_ticks;
            unsigned long blit_ticks;

            /* Complete the debug sample before reading the clock. This wait is
               gated by --gui-live-debug and never burdens normal painting. */
            WaitBlit();
            profile_blit_done = clock();
            render_ticks = (profile_start != (clock_t)-1 &&
                            profile_render_done != (clock_t)-1 &&
                            profile_render_done >= profile_start)
                ? (unsigned long)(profile_render_done - profile_start) : 0UL;
            blit_ticks = (profile_render_done != (clock_t)-1 &&
                          profile_blit_done != (clock_t)-1 &&
                          profile_blit_done >= profile_render_done)
                ? (unsigned long)(profile_blit_done - profile_render_done) : 0UL;
            sprintf(line, "paint: afa ticks render=%lu blit=%lu hz=%lu prim=%lu",
                    render_ticks, blit_ticks, (unsigned long)CLOCKS_PER_SEC,
                    tg_gui_profile_prims);
            tg_gui_log(line);
            sprintf(line,
                    "paint: afa text blits=%lu chars=%lu fallback=%lu photo rgbrows=%lu penruns=%lu",
                    tg_gui_afa_template_blits, tg_gui_afa_template_chars,
                    tg_gui_profile_afa_fallback_blits,
                    tg_gui_profile_photo_rgb_rows,
                    tg_gui_profile_photo_pen_runs);
            tg_gui_log(line);
            ++c->afa_profile_paints;
        }
        if (!first_logged) {
            tg_gui_log("paint1: blit done");
            first_logged = 1;
        }
    } else {
        if (!first_logged) {
            tg_gui_log("paint1: direct render start (no buffer)");
        }
        /* The renderer's own trail would write to disk INSIDE the lock: off. */
        tg_gui_paint_trail_off();
        if (layer != 0) {
            LockLayerRom(layer);
        }
        tg_gui_paint(state, backend);
        if (layer != 0) {
            UnlockLayerRom(layer);
        }
        tg_gui_photo_direct_report(c);
        if (!first_logged) {
            tg_gui_log("paint1: direct render done");
            first_logged = 1;
        }
    }
    tg_gui_photo_cache_visibility_changed();
}

/* Caret-only blink repaint. With the buffer, re-render just the focused strip
   into it (tg_gui_paint_caret touches only that strip), then blit the whole
   already-current buffer -- correct and flicker-free; the blink only runs while a
   field is focused, so the 2 Hz full copy is cheap. */
static void tg_gui_window_paint_caret(const tg_gui_state *state,
                                      tg_gui_backend *backend)
{
    tg_gui_amiga_ctx *c = (tg_gui_amiga_ctx *)backend->context;
    struct Layer *layer;

    if (c == 0 || c->rport == 0) {
        return;
    }
    layer = c->rport->Layer;
    if (c->buf_ok && c->buf_bm != 0 &&
        c->buf_w == c->inner_w && c->buf_h == c->inner_h) {
        struct RastPort *saved_rport = c->rport;
        int saved_ox = c->origin_x;
        int saved_oy = c->origin_y;
        int blit_x = 0;
        int blit_y = 0;
        int blit_w = c->inner_w;
        int blit_h = c->inner_h;

        WaitBlit();
        c->rport = &c->buf_rp;
        c->origin_x = 0;
        c->origin_y = 0;
        tg_gui_paint_caret(state, backend);
        c->rport = saved_rport;
        c->origin_x = saved_ox;
        c->origin_y = saved_oy;

        /* AfA needs the bitmap-font fallback, but an ordinary composer edit
           touches only the bottom input strip. Copying the complete RTG bitmap
           for every key made a PiStorm feel like a slow 030. Keep the full
           copy for popups and other modes whose dirty geometry is wider. */
        if (c->bitmap_text_compat &&
            state->mode == TG_GUI_MODE_CHAT && !state->search_active &&
            !state->mention_active && !state->ctx_visible) {
            int input_h;
            int sidebar_w;
            int content_h;

            input_h = tg_gui_input_layout_height(state, backend);
            sidebar_w = tg_gui_sidebar_w(c->inner_w);
            content_h = c->inner_h - (c->line_h + 6);
            blit_x = sidebar_w + 8;
            blit_y = content_h - input_h;
            blit_w = c->inner_w - blit_x - 8;
            blit_h = input_h;
            if (blit_x < 0 || blit_y < 0 || blit_w <= 0 || blit_h <= 0 ||
                blit_x + blit_w > c->inner_w ||
                blit_y + blit_h > c->inner_h) {
                blit_x = 0;
                blit_y = 0;
                blit_w = c->inner_w;
                blit_h = c->inner_h;
            }
        }
        if (layer != 0) {
            LockLayerRom(layer);
        }
        BltBitMapRastPort(c->buf_bm, blit_x, blit_y, c->rport,
                          saved_ox + blit_x, saved_oy + blit_y,
                          blit_w, blit_h, 0xC0);
        (void)tg_gui_photo_direct_replay(c, blit_x, blit_y,
                                         blit_w, blit_h);
        if (layer != 0) {
            UnlockLayerRom(layer);
        }
        tg_gui_photo_direct_report(c);
    } else {
        if (layer != 0) {
            LockLayerRom(layer);
        }
        tg_gui_paint_caret(state, backend);
        if (layer != 0) {
            UnlockLayerRom(layer);
        }
        tg_gui_photo_direct_report(c);
    }
}

/* AfA's bitmap-font fallback is still more expensive than native Text(), even
   after batching glyphs into run templates. During ordinary composer edits,
   redraw only the input strip when its height and popup footprint did not
   change. Other systems retain the established full-paint behaviour; their
   native buffered Text() path is already fast. */
static void tg_gui_window_paint_composer_edit(tg_gui_state *state,
                                               tg_gui_backend *backend,
                                               int old_input_h,
                                               int old_mention_active)
{
    tg_gui_amiga_ctx *ctx;
    int new_input_h;

    ctx = (tg_gui_amiga_ctx *)backend->context;
    new_input_h = tg_gui_input_layout_height(state, backend);
    if (ctx != 0 && ctx->bitmap_text_compat &&
        !old_mention_active && !state->mention_active &&
        old_input_h > 0 && old_input_h == new_input_h) {
        tg_gui_window_paint_caret(state, backend);
    } else {
        tg_gui_window_paint(state, backend);
    }
}

/* How many extra older pages the open may auto-pull to make the backlog overflow
   the window (so a scrollbar appears) when the first page kept only a few text
   rows. Bounded per platform: MorphOS smallest (bsdsocket freeze risk on many
   replies), m68k modest, PPC/AROS a bit more. */
#if defined(__MORPHOS__) || defined(__MORPHOS)
#define TG_GUI_TOPUP_MAX 1
#elif defined(__m68k__)
#define TG_GUI_TOPUP_MAX 2
#else
#define TG_GUI_TOPUP_MAX 3
#endif

/* Switch to chat `sel`: show its header + an empty transcript at once, then
   fetch the history. The instant first paint keeps the switch responsive on a
   slow link instead of the window appearing frozen on the old chat until the
   load finishes. */
static void tg_gui_window_open_selection(tg_gui_state *state, int sel,
                                         tg_gui_backend *backend)
{
    /* Hard bounds guard: with a reprojected (possibly emptied) sidebar a
       stale sel would read garbage from chats[] and open a nonexistent peer
       -- the silent half of the "remove leaves the app stuck" report. */
    if (sel < 0 || sel >= state->chat_count) {
        tg_gui_window_paint(state, backend);
        return;
    }
    state->selected_chat = sel;
    state->nav_chat = -1;     /* the arrow focus is consumed by the open */
    state->selected_msg = -1; /* new chat: no message highlighted yet */
    state->transcript_scroll = 0; /* a freshly opened chat pins to the newest */
    state->chat_scroll_to_sel = 1; /* scroll the sidebar so the row is visible */
    /* Opening a chat clears its unread badge / flash -- you are now reading it. */
    state->chats[sel].unread = 0;
    state->chats[sel].flash = 0;
    tg_gui_window_apply_selection(state);
    if (tg_gui_session_is_open()) {
        state->message_count = 0;
        state->msg_gen++;
        tg_gui_window_paint(state, backend);
        tg_gui_log("open_selection: open_chat begin");
        (void)tg_gui_session_open_chat(state->chats[sel].index, stdout);
    }
    tg_gui_window_paint(state, backend);
    /* Auto-top-up: some chats keep only a few text rows at open (service/empty
       messages dropped, or a media-tail aborting the parse), so the backlog fits
       the window and no scrollbar is drawn -- the user then can't tell there is
       more history. While the content fits (sb_tr_max==0) and the chat start is
       not reached, pull older pages via the proven load_older path (newest stays
       on screen, so allow_drop=0) until it overflows and a scrollbar appears;
       normal scroll/wheel paging takes over from there. Bounded per platform.
       transcript_scroll stays 0 so the newest message remains pinned at bottom. */
    if (tg_gui_session_is_open()) {
        int topup;

        for (topup = 0; topup < TG_GUI_TOPUP_MAX; ++topup) {
            if (state->sb_tr_max > 0) {
                break; /* already overflows -> a scrollbar is present */
            }
            if (tg_gui_session_load_older(stdout, 0) <= 0) {
                break; /* chat start reached (0) or transient fetch failure (<0) */
            }
            tg_gui_window_paint(state, backend);
        }
    }
    /* The unread badge was just cleared (you are reading this chat) -- persist it
       so it does not snap back to the snapshot count after a restart. */
    tg_gui_session_persist_unread();
}

/* Jump the open transcript to the true newest message (Telegram's down-arrow).
   If the ring-bottom is STALE (newest_dropped: a load-older paging evicted the
   true-newest tail), the only way back is to RELOAD via open_selection -- it
   re-fetches the newest history, exactly what re-entering the chat does, and
   tg_gui_session_open_chat then clears the flags centrally. Otherwise the newest
   is already in the ring, so just re-pin (transcript_scroll = 0). */
static void tg_gui_window_jump_to_bottom(tg_gui_state *state,
                                         tg_gui_backend *backend,
                                         int *older_exhausted,
                                         int *older_cooldown)
{
    if (state == 0) {
        return;
    }
    if (state->newest_dropped && state->selected_chat >= 0 &&
        state->selected_chat < state->chat_count && tg_gui_session_is_open()) {
        /* Reload path. open_selection on the SAME chat does not change
           selected_chat, so the loop's open-time re-arm of the paging latches
           would not fire -- reset them here. */
        tg_gui_window_open_selection(state, state->selected_chat, backend);
        if (older_exhausted != 0) {
            *older_exhausted = 0;
        }
        if (older_cooldown != 0) {
            *older_cooldown = 0;
        }
    } else {
        state->transcript_scroll = 0;
        state->unread_below = 0;
        state->newest_dropped = 0;
        tg_gui_window_paint(state, backend);
    }
}

/* Bounded copy into a fixed UI buffer (status/title) -- never overflows even if
   a future string grows past the field. */
static void tg_gui_window_copy(char *dest, unsigned long size, const char *src)
{
    unsigned long i;

    if (dest == 0 || size == 0UL) {
        return;
    }
    for (i = 0UL; i + 1UL < size && src != 0 && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/* Push a just-sent line into the composer recall ring (UP/DOWN). Skips empty and
   consecutive duplicates, shifts the ring when full, resets the recall cursor.
   Mirrors the TUI's tg_chat_history_add. */
static void tg_gui_history_add(tg_gui_state *state, const char *text)
{
    int i;

    state->history_pos = -1;
    if (text == 0 || text[0] == '\0') {
        return;
    }
    if (state->history_count > 0 &&
        strcmp(state->history[state->history_count - 1], text) == 0) {
        return;
    }
    if (state->history_count >= TG_GUI_HISTORY_MAX) {
        for (i = 1; i < TG_GUI_HISTORY_MAX; ++i) {
            tg_gui_window_copy(state->history[i - 1], TG_GUI_TEXT_MAX,
                               state->history[i]);
        }
        state->history_count = TG_GUI_HISTORY_MAX - 1;
    }
    tg_gui_window_copy(state->history[state->history_count], TG_GUI_TEXT_MAX,
                       text);
    ++state->history_count;
}

/* Brings the window live after a successful login: opens the session over the
   freshly-written auth.bin (activate flips state->mode to chat + opens the first
   chat). If the re-open fails, the login itself still succeeded (auth.bin is
   written), so drop to chat mode offline rather than trapping the user back in
   the login screen -- where login.active is now cleared and every retry would
   just error. A relaunch will connect. */
static void tg_gui_window_login_finish(tg_gui_state *state)
{
    if (tg_gui_session_login_activate(state, stdout) == 0) {
        return;
    }
    state->mode = TG_GUI_MODE_CHAT;
    state->composing = 0;
    state->input_masked = 0;
    state->input[0] = '\0';
    tg_gui_window_copy(state->title, sizeof(state->title), "Telegram Amiga");
    tg_gui_window_copy(state->status, sizeof(state->status),
                       "Logged in - relaunch to connect");
}

/* Handles one key while a login screen is shown (state->mode != CHAT). ESC
   aborts the window; printable keys edit the field; RETURN submits the field
   to the matching auth step and advances the screen (or shows an error). The
   network round-trip blocks, so a "Connessione..." status is painted first. */

/* The login code prompt, with Telegram's own answer about where it sent the
   code (inside the app, SMS, a call) and how many digits it has when the
   server said so. */
static void tg_gui_window_login_code_prompt(tg_gui_state *state)
{
    const char *hint = tg_mtproto_sent_code_hint();
    unsigned long digits = tg_mtproto_sent_code_length();
    char line[TG_GUI_NAME_MAX + 16];

    /* The status line is short: keep the hint inside it and only add the
       digit count when there is room for it. */
    if (digits > 0UL && digits < 100UL) {
        sprintf(line, "%.34s (%lu digits)", hint, digits);
    } else {
        sprintf(line, "%.46s", hint);
    }
    tg_gui_window_copy(state->status, sizeof(state->status), line);
}

static void tg_gui_window_login_key(tg_gui_state *state, UWORD code,
                                    tg_gui_backend *backend, int *done,
                                    int *caret_ticks)
{
    if (code == 27) { /* ESC: give up on logging in */
        memset(state->input, 0, sizeof(state->input)); /* wipe any typed secret */
        state->input_masked = 0;
        *done = 1;
        return;
    }
    if (code == 8 || code == 127) { /* BACKSPACE */
        unsigned long n;

        n = (unsigned long)strlen(state->input);
        if (n > 0UL) {
            state->input[n - 1UL] = '\0';
            tg_gui_window_paint(state, backend);
        }
        return;
    }
    if (code != 13 && code != 10) { /* a printable character */
        if (code >= 32 && code < 256) {
            unsigned long n;

            n = (unsigned long)strlen(state->input);
            if (n + 1UL < (unsigned long)sizeof(state->input)) {
                state->input[n] = (char)code;
                state->input[n + 1UL] = '\0';
                tg_gui_window_paint(state, backend);
            }
        }
        return;
    }

    /* Never submit an EMPTY phone/code field. sendCode/signIn block for several
       seconds, during which a held/auto-repeating RETURN queues a second submit;
       once the step has advanced to the code, that stray RETURN fired signIn with
       an empty code -> a cryptic "query-build-failed" before the user could type
       it (the code had already arrived on the phone). Re-prompt instead.
       LOGIN_2FA is DELIBERATELY excluded: an empty submit there is the legitimate
       way to finish login on an account with no 2FA password -- check_password
       asks the server (account.getPassword) and the "no password required"
       shortcut completes the login. Blocking it traps such users in a re-prompt
       loop if they ever land on the 2FA screen. A real 2FA account just gets a
       harmless "password-invalid" and re-prompts. */
    if ((state->mode == TG_GUI_MODE_LOGIN_PHONE ||
         state->mode == TG_GUI_MODE_LOGIN_CODE) &&
        state->input[0] == '\0') {
        if (state->mode == TG_GUI_MODE_LOGIN_PHONE) {
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Enter your phone (+ country code)");
        } else {
            tg_gui_window_login_code_prompt(state);
        }
        state->cursor_on = 1;
        *caret_ticks = 0;
        tg_gui_window_paint(state, backend);
        return;
    }

    /* RETURN: submit the current field. Show progress first -- the DH/RPC round
       trip blocks the window for several seconds on a slow link. */
    tg_gui_window_copy(state->status, sizeof(state->status),
                       "Connecting to Telegram...");
    state->cursor_on = 0;
    tg_gui_window_paint(state, backend);

    if (state->mode == TG_GUI_MODE_LOGIN_PHONE) {
        int rc;

        rc = tg_gui_session_login_send_code(state->input, stdout);
        state->input[0] = '\0';
        if (rc == TG_GUI_LOGIN_OK) {
            state->mode = TG_GUI_MODE_LOGIN_CODE;
            /* Say WHERE the code went. Telegram sends it inside the app when
               another device is signed in, and someone waiting for an SMS
               concludes this client is broken (field question). */
            tg_gui_window_login_code_prompt(state);
        } else {
            const char *e = tg_gui_session_login_last_error();
            tg_gui_window_copy(state->status, sizeof(state->status),
                               (e != 0 && e[0] != '\0')
                                   ? e : "Invalid number - try again");
        }
    } else if (state->mode == TG_GUI_MODE_LOGIN_CODE) {
        int rc;

        rc = tg_gui_session_login_sign_in(state->input, stdout);
        state->input[0] = '\0';
        if (rc == TG_GUI_LOGIN_OK) {
            tg_gui_window_login_finish(state);
        } else if (rc == TG_GUI_LOGIN_NEED_2FA) {
            state->mode = TG_GUI_MODE_LOGIN_2FA;
            state->input_masked = 1;
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "2FA password (Enter if you have none)");
        } else if (rc == TG_GUI_LOGIN_BAD_CODE) {
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Wrong code - try again");
            state->mode = TG_GUI_MODE_LOGIN_CODE;
        } else {
            const char *e = tg_gui_session_login_last_error();
            tg_gui_window_copy(state->status, sizeof(state->status),
                               (e != 0 && e[0] != '\0')
                                   ? e : "Error - try the code again");
        }
    } else { /* TG_GUI_MODE_LOGIN_2FA */
        int rc;

        rc = tg_gui_session_login_check_password(state->input, stdout);
        memset(state->input, 0, sizeof(state->input)); /* wipe the password */
        if (rc == TG_GUI_LOGIN_OK) {
            state->input_masked = 0;
            tg_gui_window_login_finish(state);
        } else if (rc == TG_GUI_LOGIN_BAD_PASSWORD) {
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Wrong password - try again");
        } else {
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Error - try the password again");
        }
    }
    state->cursor_on = 1;
    *caret_ticks = 0;
    tg_gui_window_paint(state, backend);
}

/* The right-button menu strip (laid out by GadTools so the metrics follow the
   screen font). Quit also gets the standard Right-Amiga+Q shortcut. */
/* ---- clipboard.device, zero-dep IFF-FTXT ---------------------------------
   Copy/paste for issue #5. The IFF is built and parsed BY HAND (FORM/FTXT/
   CHRS) with explicit big-endian 32-bit sizes: IFF is big-endian on the
   clipboard while the AROS lanes are little-endian CPUs. Unit 0, the one
   every Amiga clipboard tool shares. */
#if defined(TG_GUI_AMIGA)
static void tg_gui_clip_u32be(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned long tg_gui_clip_u32be_read(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static struct IOClipReq *tg_gui_clip_open(struct MsgPort **port_out)
{
    struct MsgPort *port;
    struct IOClipReq *io;

    port = CreateMsgPort();
    if (port == 0) {
        return 0;
    }
    io = (struct IOClipReq *)CreateIORequest(port, sizeof(struct IOClipReq));
    if (io == 0 ||
        OpenDevice((CONST_STRPTR)"clipboard.device", 0,
                   (struct IORequest *)io, 0) != 0) {
        if (io != 0) {
            DeleteIORequest((struct IORequest *)io);
        }
        DeleteMsgPort(port);
        return 0;
    }
    *port_out = port;
    return io;
}

static void tg_gui_clip_close(struct IOClipReq *io, struct MsgPort *port)
{
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
}

/* Writes `text` to clip unit 0 as FORM FTXT / CHRS. 1 = ok. */
/* Pairs are a screen thing: the clipboard gets the text emoticon instead,
   which is what any other Amiga program can show. */
static void tg_gui_clip_expand_pairs(const char *src, char *dst,
                                     unsigned long dst_size)
{
    unsigned long i = 0UL;
    unsigned long o = 0UL;
    unsigned long len = (unsigned long)strlen(src);
    unsigned long index;

    while (i < len && o + 1UL < dst_size) {
        if (tg_gui_emoji_pair_at(src, len, i, &index)) {
            const char *t = tg_gui_session_emoji_text(index);

            while (*t != '\0' && o + 1UL < dst_size) {
                dst[o++] = *t++;
            }
            i += 2UL;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
}

static int tg_gui_clip_write_text_raw(const char *text);
static int tg_gui_clip_write_text(const char *text)
{
    static char expanded[TG_GUI_MSG_TEXT_MAX + 64];

    if (text == 0) {
        return 0;
    }
    tg_gui_clip_expand_pairs(text, expanded, sizeof(expanded));
    return tg_gui_clip_write_text_raw(expanded);
}

static int tg_gui_clip_write_text_raw(const char *text)
{
    static unsigned char iff[TG_GUI_MSG_TEXT_MAX + 24];
    struct MsgPort *port;
    struct IOClipReq *io;
    unsigned long tlen, even;
    int ok = 0;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    tlen = (unsigned long)strlen(text);
    if (tlen > (unsigned long)TG_GUI_MSG_TEXT_MAX) {
        tlen = TG_GUI_MSG_TEXT_MAX;
    }
    even = tlen + (tlen & 1UL);
    memcpy(iff, "FORM", 4);
    tg_gui_clip_u32be(iff + 4, 4UL + 8UL + even); /* FTXT + CHRS hdr + data */
    memcpy(iff + 8, "FTXT", 4);
    memcpy(iff + 12, "CHRS", 4);
    tg_gui_clip_u32be(iff + 16, tlen);
    memcpy(iff + 20, text, tlen);
    if (tlen & 1UL) {
        iff[20 + tlen] = 0; /* IFF pad byte */
    }
    io = tg_gui_clip_open(&port);
    if (io == 0) {
        return 0;
    }
    io->io_Offset = 0;
    io->io_ClipID = 0;
    io->io_Command = CMD_WRITE;
    io->io_Data = (STRPTR)iff;
    io->io_Length = (LONG)(20UL + even);
    if (DoIO((struct IORequest *)io) == 0) {
        io->io_Command = CMD_UPDATE;
        ok = DoIO((struct IORequest *)io) == 0;
    }
    tg_gui_clip_close(io, port);
    return ok;
}

/* One positioned CMD_READ helper; the device advances io_Offset itself. */
static long tg_gui_clip_read(struct IOClipReq *io, void *buf,
                             unsigned long len)
{
    io->io_Command = CMD_READ;
    io->io_Data = (STRPTR)buf;
    io->io_Length = (LONG)len;
    if (DoIO((struct IORequest *)io) != 0) {
        return -1;
    }
    return (long)io->io_Actual;
}

/* Reads the first CHRS chunk of a FORM FTXT clip into out (NUL-terminated).
   Returns the number of bytes copied (0 = empty clip / not text). */
static unsigned long tg_gui_clip_read_text(char *out, unsigned long out_size)
{
    struct MsgPort *port;
    struct IOClipReq *io;
    unsigned char hdr[12];
    unsigned long copied = 0;

    if (out == 0 || out_size == 0UL) {
        return 0;
    }
    out[0] = '\0';
    io = tg_gui_clip_open(&port);
    if (io == 0) {
        return 0;
    }
    io->io_Offset = 0;
    io->io_ClipID = 0;
    if (tg_gui_clip_read(io, hdr, 12UL) == 12L &&
        memcmp(hdr, "FORM", 4) == 0 && memcmp(hdr + 8, "FTXT", 4) == 0) {
        for (;;) {
            unsigned char chdr[8];
            unsigned long clen;

            if (tg_gui_clip_read(io, chdr, 8UL) != 8L) {
                break;
            }
            clen = tg_gui_clip_u32be_read(chdr + 4);
            if (memcmp(chdr, "CHRS", 4) == 0 && copied == 0UL) {
                unsigned long want = clen;

                if (want > out_size - 1UL) {
                    want = out_size - 1UL;
                }
                if (want > 0UL &&
                    tg_gui_clip_read(io, out, want) == (long)want) {
                    copied = want;
                }
                out[copied] = '\0';
                break; /* first text chunk is all we paste */
            }
            /* skip a foreign chunk (+ IFF pad) by dummy reads */
            {
                static unsigned char sink[256];
                unsigned long skip = clen + (clen & 1UL);

                while (skip > 0UL) {
                    unsigned long step = skip > sizeof(sink) ? sizeof(sink)
                                                             : skip;

                    if (tg_gui_clip_read(io, sink, step) <= 0L) {
                        skip = 0UL;
                        break;
                    }
                    skip -= step;
                }
            }
        }
    }
    /* drain to the end so the device releases the clip */
    {
        static unsigned char sink[256];

        while (tg_gui_clip_read(io, sink, sizeof(sink)) > 0L) {
        }
    }
    tg_gui_clip_close(io, port);
    return copied;
}
#endif /* TG_GUI_AMIGA */

/* Cut/copy/paste live in their own Edit menu per the AmigaOS UI Style Guide
   (an issue #5 follow-up); the MENUPICK handler keys off GTMENUITEM_USERDATA,
   so item positions are free to move. */
static struct NewMenu tg_gui_newmenu[] = {
/* Project */
    { NM_TITLE, (STRPTR)"Telegram", 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Iconify", (STRPTR)"I", 0, 0,
      (APTR)TG_MENU_ICONIFY },
    { NM_ITEM,  (STRPTR)"About...", 0, 0, 0,
      (APTR)TG_MENU_ABOUT },
    { NM_ITEM,  (STRPTR)"Help...",  0, 0, 0,
      (APTR)TG_MENU_HELP },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Remove chat from list", (STRPTR)"R", 0, 0,
      (APTR)TG_MENU_REMOVE },
    { NM_ITEM,  (STRPTR)"Reload chat list", 0, 0, 0,
      (APTR)TG_MENU_RELOAD },
    { NM_ITEM,  (STRPTR)"Send file...", (STRPTR)"F", 0, 0,
      (APTR)TG_MENU_SENDFILE },
    { NM_ITEM,  (STRPTR)"Send photo...", (STRPTR)"P", 0, 0,
      (APTR)TG_MENU_SENDPHOTO },
    { NM_ITEM,  (STRPTR)"Insert emoji...", (STRPTR)"E", 0, 0,
      (APTR)TG_MENU_EMOJI },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Quit", (STRPTR)"Q", 0, 0,
      (APTR)TG_MENU_QUIT },
/* Edit */
    { NM_TITLE, (STRPTR)"Edit", 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Cut", (STRPTR)"X", 0, 0,
      (APTR)TG_MENU_CUT },
    { NM_ITEM,  (STRPTR)"Copy", (STRPTR)"C", 0, 0,
      (APTR)TG_MENU_COPY },
    { NM_ITEM,  (STRPTR)"Paste", (STRPTR)"V", 0, 0,
      (APTR)TG_MENU_PASTE },
/* Settings */
    { NM_TITLE, (STRPTR)"Settings", 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Use own screen", 0, CHECKIT | MENUTOGGLE, 0,
      (APTR)TG_MENU_OWNSCREEN },
    { NM_ITEM,  (STRPTR)"Download drawer...", 0, 0, 0,
      (APTR)TG_MENU_DLDIR },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Show inline photos", 0, CHECKIT | MENUTOGGLE, 0,
      (APTR)TG_MENU_INLINEPHOTOS },
    { NM_ITEM,  (STRPTR)"Photo dithering", 0, 0, 0, 0 },
    { NM_SUB,   (STRPTR)"Full", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_DITHER_FULL },
    { NM_SUB,   (STRPTR)"Light", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_DITHER_LIGHT },
    { NM_SUB,   (STRPTR)"Off", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_DITHER_OFF },
    { NM_ITEM,  (STRPTR)"Photo cache limit", 0, 0, 0, 0 },
    { NM_SUB,   (STRPTR)"10 MB", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_CACHE_10 },
    { NM_SUB,   (STRPTR)"50 MB", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_CACHE_50 },
    { NM_SUB,   (STRPTR)"200 MB", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_CACHE_200 },
    { NM_SUB,   (STRPTR)"Unlimited", 0,
      CHECKIT | MENUTOGGLE, 0, (APTR)TG_MENU_CACHE_UNLIMITED },
    { NM_SUB,   NM_BARLABEL, 0, 0, 0, 0 },
    { NM_SUB,   (STRPTR)"Clear cache...", 0, 0, 0,
      (APTR)TG_MENU_CACHE_CLEAR },

    { NM_END,   0, 0, 0, 0, 0 }
};

static struct MenuItem *tg_gui_menu_find_userdata(struct Menu *menu, APTR data)
{
    struct Menu *m;

    for (m = menu; m != 0; m = m->NextMenu) {
        struct MenuItem *item;

        for (item = m->FirstItem; item != 0; item = item->NextItem) {
            struct MenuItem *sub;

            if (GTMENUITEM_USERDATA(item) == data) {
                return item;
            }
            for (sub = item->SubItem; sub != 0; sub = sub->NextItem) {
                if (GTMENUITEM_USERDATA(sub) == data) {
                    return sub;
                }
            }
        }
    }
    return 0;
}

static void tg_gui_menu_set_photo_dither(struct Menu *menu, int dither)
{
    static APTR const ids[3] = {
        (APTR)TG_MENU_DITHER_FULL, (APTR)TG_MENU_DITHER_LIGHT,
        (APTR)TG_MENU_DITHER_OFF
    };
    int selected;
    int i;

    selected = dither == TG_GUI_PHOTO_DITHER_LIGHT ? 1 :
               dither == TG_GUI_PHOTO_DITHER_OFF ? 2 : 0;
    for (i = 0; i < 3; ++i) {
        struct MenuItem *item;

        item = tg_gui_menu_find_userdata(menu, ids[i]);
        if (item != 0) {
            if (i == selected) {
                item->Flags |= CHECKED;
            } else {
                item->Flags &= (UWORD)~CHECKED;
            }
        }
    }
}

static void tg_gui_menu_set_photo_cache_limit(struct Menu *menu,
                                               unsigned long limit_mb)
{
    static APTR const ids[4] = {
        (APTR)TG_MENU_CACHE_10, (APTR)TG_MENU_CACHE_50,
        (APTR)TG_MENU_CACHE_200, (APTR)TG_MENU_CACHE_UNLIMITED
    };
    int selected;
    int i;

    selected = limit_mb == 10UL ? 0 : limit_mb == 200UL ? 2 :
               limit_mb == TG_GUI_PHOTO_CACHE_UNLIMITED_MB ? 3 : 1;
    for (i = 0; i < 4; ++i) {
        struct MenuItem *item;

        item = tg_gui_menu_find_userdata(menu, ids[i]);
        if (item != 0) {
            if (i == selected) {
                item->Flags |= CHECKED;
            } else {
                item->Flags &= (UWORD)~CHECKED;
            }
        }
    }
}

static const char tg_gui_about_text[] =
    "Telegram Amiga\n"
    "alpha " TG_VERSION "  (built " __DATE__ ")\n\n"
    "A native Telegram client for AmigaOS,\n"
    "MorphOS and AROS.\n\n"
    "by Michele Dipace\n"
    "michele.dipace@kaffeine.net\n\n"
    "Contributions: Javier de las Rivas (javierdlr)\n\n"
    "And thanks to the testers around the world\n"
    "who run this on real hardware and send back\n"
    "what they find. This client is what it is\n"
    "because of them.\n\n"
    "Source, issues and new releases:\n"
    "github.com/kaffeine1/telegram-amiga";

static const char tg_gui_help_text[] =
    "Chat selection:\n"
    "  F1 - F10          chats 1 to 10\n"
    "  Shift + F1 - F10  chats 11 to 20\n\n"
    "ENTER        write a message to the open chat\n"
    "Click msg    select it (A+C copies); double-click replies\n"
    "Del / A+R    remove the selected chat from the list\n"
    "A+F          send a file to the open chat\n"
    "A+P          send a JPEG as a photo\n"
    "A+I          iconify to an AppIcon (double-click it to return)\n"
    "ESC          cancel\n"
    "Q            quit";

/* EasyRequest runs its OWN modal input loop and never answers our
   IDCMP_MENUVERIFY handshake, so a right-click while it is up would stall every
   app's menus on the whole screen (RKRM: drop the verify bits around a
   requester). Bracket the call with ModifyIDCMP and restore the original flags
   after. */
static LONG tg_gui_amiga_easyreq_args(struct Window *win, struct EasyStruct *es)
{
    ULONG saved = win->IDCMPFlags;
    LONG result;

    if ((saved & IDCMP_MENUVERIFY) != 0UL) {
        ModifyIDCMP(win, saved & ~(ULONG)IDCMP_MENUVERIFY);
    }
    result = EasyRequestArgs(win, es, 0, 0);
    if ((saved & IDCMP_MENUVERIFY) != 0UL) {
        ModifyIDCMP(win, saved);
    }
    return result;
}

/* EasyRequestArgs does not map ESC to its rightmost gadget. This small
   requester loop preserves the standard gadget return values while making
   raw ESC an explicit zero (Cancel) on every Intuition-compatible target. */
static LONG tg_gui_amiga_easyreq_cancel_args(struct Window *win,
                                             struct EasyStruct *es)
{
    ULONG saved;
    struct Window *requester;
    LONG result;

    saved = win->IDCMPFlags;
    if ((saved & IDCMP_MENUVERIFY) != 0UL) {
        ModifyIDCMP(win, saved & ~(ULONG)IDCMP_MENUVERIFY);
    }
    requester = BuildEasyRequestArgs(win, es, IDCMP_RAWKEY, 0);
    result = requester == (struct Window *)1 ? 1L : 0L;
    if (requester != 0 && requester != (struct Window *)1) {
        /* SysReqHandler owns the gadget mapping. Reading it ourselves (from
           the gadget id, then from the button geometry) sent MorphOS photos
           out as files: only the system knows how ITS requester gadgets are
           laid out and numbered. The documented return is the classic
           1,2,...,N left to right with the rightmost 0 -- exactly what the
           callers expect. -1 means our IDCMP_RAWKEY arrived without
           satisfying the requester: a key press, ESC among them, so it
           cancels (the sole reason this loop exists instead of a plain
           EasyRequestArgs). */
        ULONG idcmp;

        for (;;) {
            idcmp = IDCMP_RAWKEY;
            result = SysReqHandler(requester, &idcmp, TRUE);
            if (result >= 0L) {
                break;
            }
            if (result == -1L) {
                result = 0L; /* keystroke -> cancel */
                break;
            }
            /* -2: input that did not satisfy the requester; keep waiting. */
        }
        if (tg_gui_log_is_enabled()) {
            char line[48];

            sprintf(line, "req: result %ld", (long)result);
            tg_gui_log(line);
        }
    }
    FreeSysRequest(requester);
    if ((saved & IDCMP_MENUVERIFY) != 0UL) {
        ModifyIDCMP(win, saved);
    }
    return result;
}

/* Shows a one-button info requester (About / Help). No printf args, so the
   text is passed verbatim (it carries no '%'). */
static void tg_gui_amiga_easyreq(struct Window *win, const char *title,
                                 const char *body)
{
    struct EasyStruct es;

    es.es_StructSize = (ULONG)sizeof(struct EasyStruct);
    es.es_Flags = 0UL;
    es.es_Title = (STRPTR)title;
    es.es_TextFormat = (STRPTR)body;
    es.es_GadgetFormat = (STRPTR)"OK";
    (void)tg_gui_amiga_easyreq_args(win, &es);
}

/* Two-button confirm for removing a chat. Returns 1 = Remove, 0 = Cancel. The
   chat name is baked into the body with any '%' dropped, so EasyRequest (which
   treats es_TextFormat as a printf format) never reads phantom args -- safer than
   passing the name as a pointer arg, which would also be size-fragile on 64-bit
   AROS. */
static int tg_gui_amiga_confirm_remove(struct Window *win, const char *name)
{
    struct EasyStruct es;
    char body[TG_GUI_NAME_MAX + 80];
    const char *pre = "Remove this chat from the list?\n\n";
    const char *post = "\n\n(re-add it later via Search)";
    const char *p;
    int n;

    n = 0;
    for (p = pre; *p != '\0' && n < (int)sizeof(body) - 1; ++p) {
        body[n++] = *p;
    }
    if (name != 0) {
        for (p = name; *p != '\0' && n < (int)sizeof(body) - 1; ++p) {
            if (*p != '%') {
                body[n++] = *p;
            }
        }
    }
    for (p = post; *p != '\0' && n < (int)sizeof(body) - 1; ++p) {
        body[n++] = *p;
    }
    body[n] = '\0';

    es.es_StructSize = (ULONG)sizeof(struct EasyStruct);
    es.es_Flags = 0UL;
    es.es_Title = (STRPTR)"Remove chat";
    es.es_TextFormat = (STRPTR)body;
    es.es_GadgetFormat = (STRPTR)"Remove|Cancel";
    return (int)tg_gui_amiga_easyreq_args(win, &es);
}

/* One-line confirm before deleting a message for everyone. 1 = Delete. */
static int tg_gui_amiga_confirm_delete(struct Window *win)
{
    struct EasyStruct es;

    es.es_StructSize = (ULONG)sizeof(struct EasyStruct);
    es.es_Flags = 0UL;
    es.es_Title = (STRPTR)"Delete message";
    es.es_TextFormat = (STRPTR)"Delete this message for everyone?";
    es.es_GadgetFormat = (STRPTR)"Delete|Cancel";
    return (int)tg_gui_amiga_easyreq_args(win, &es);
}

/* Save requesters never replace an existing photo without an explicit yes. */
static int tg_gui_amiga_confirm_photo_overwrite(struct Window *win)
{
    struct EasyStruct es;

    es.es_StructSize = (ULONG)sizeof(struct EasyStruct);
    es.es_Flags = 0UL;
    es.es_Title = (STRPTR)"Replace photo";
    es.es_TextFormat = (STRPTR)"That file already exists. Replace it?";
    es.es_GadgetFormat = (STRPTR)"Replace|Cancel";
    return (int)tg_gui_amiga_easyreq_cancel_args(win, &es);
}

/* Cache clear is destructive only for reproducible thumbnails/canonical files;
   ESC maps to No through the same requester loop used by JPEG drop choices. */
static int tg_gui_amiga_confirm_clear_photo_cache(struct Window *win)
{
    struct EasyStruct es;

    es.es_StructSize = (ULONG)sizeof(struct EasyStruct);
    es.es_Flags = 0UL;
    es.es_Title = (STRPTR)"Clear photo cache";
    es.es_TextFormat =
        (STRPTR)"Delete downloaded photos?\n\nAvatars are not affected.";
    es.es_GadgetFormat = (STRPTR)"Yes|No";
    return (int)tg_gui_amiga_easyreq_cancel_args(win, &es);
}

/* Send-photo dialog (0.0.91 field feedback): the composer-as-caption
   question was invisible until you knew the trick. This mirrors the desktop
   client instead: one small window with the photo's preview, a caption line
   prefilled with the composer draft (the desktop moves the draft into its
   media box the same way), and Photo / File / Cancel as explicit actions.
   Preview pixels need a validated RGB target (the same check the photo
   pipeline uses); anywhere else the box shows the file's name, dimensions
   and size instead, which still beats a bare Yes/No requester. Modal by
   design, exactly like the ASL requester that preceded it in the flow.
   Returns 0 cancel, 1 send as photo, 2 send as file; fills caption. */

typedef struct tg_gui_sendphoto_ui {
    struct Window *win;
    tg_gui_amiga_ctx *main_ctx;
    const char *name;          /* bare filename for the info line */
    unsigned long bytes;       /* file size for the info line */
    char *caption;
    unsigned long caption_size;
    unsigned long caption_len;
    int iw, ih;                /* window inner size */
    int cap_y;                 /* caption box top (inner coords) */
    int btn_y;                 /* buttons row top */
    int btn_x[3], btn_w[3];    /* Photo / File / Cancel boxes */
    int cursor_on;
    int tick_count;            /* caret cadence: toggle every 5 ticks, like
                                  the composer and the search box */
} tg_gui_sendphoto_ui;


static void tg_gui_sendphoto_text(tg_gui_sendphoto_ui *ui, int pen, int x,
                                  int baseline, const char *text,
                                  unsigned long len)
{
    struct RastPort *rp = ui->win->RPort;

    SetAPen(rp, ui->main_ctx->pens[pen]);
    SetBPen(rp, ui->main_ctx->pens[TG_GUI_PEN_WINDOW]);
    SetDrMd(rp, JAM1);
    Move(rp, ui->win->BorderLeft + x, ui->win->BorderTop + baseline);
    Text(rp, (STRPTR)text, (UWORD)len);
}

static void tg_gui_sendphoto_box(tg_gui_sendphoto_ui *ui, int pen, int x,
                                 int y, int w, int h)
{
    struct RastPort *rp = ui->win->RPort;

    SetAPen(rp, ui->main_ctx->pens[pen]);
    RectFill(rp, ui->win->BorderLeft + x, ui->win->BorderTop + y,
             ui->win->BorderLeft + x + w - 1, ui->win->BorderTop + y + h - 1);
}

/* The caption row: box, the tail of the text that fits, the caret. */
static void tg_gui_sendphoto_caption_row(tg_gui_sendphoto_ui *ui)
{
    struct RastPort *rp = ui->win->RPort;
    int lh = ui->main_ctx->line_h;
    int box_w = ui->iw - 16;
    unsigned long start;
    int ascent;

    tg_gui_sendphoto_box(ui, TG_GUI_PEN_SURFACE, 8, ui->cap_y, box_w, lh + 6);
    start = 0UL;
    while (start < ui->caption_len &&
           (int)TextLength(rp, (STRPTR)(ui->caption + start),
                           (UWORD)(ui->caption_len - start)) > box_w - 14) {
        ++start; /* keep the END visible while typing */
    }
    ascent = (rp->Font != 0) ? (int)rp->Font->tf_Baseline : lh - 2;
    if (ui->caption_len > start) {
        SetAPen(rp, ui->main_ctx->pens[TG_GUI_PEN_TEXT]);
        SetDrMd(rp, JAM1);
        Move(rp, ui->win->BorderLeft + 12,
             ui->win->BorderTop + ui->cap_y + 3 + ascent);
        Text(rp, (STRPTR)(ui->caption + start),
             (UWORD)(ui->caption_len - start));
    }
    if (ui->cursor_on) {
        int cx = 12 + (int)TextLength(rp, (STRPTR)(ui->caption + start),
                                      (UWORD)(ui->caption_len - start)) + 1;

        tg_gui_sendphoto_box(ui, TG_GUI_PEN_TEXT, cx, ui->cap_y + 3, 2, lh);
    }
}

static void tg_gui_sendphoto_paint(tg_gui_sendphoto_ui *ui)
{
    struct RastPort *rp = ui->win->RPort;
    int lh = ui->main_ctx->line_h;
    static const char *labels[3];
    int i;

    labels[0] = "Photo";
    labels[1] = "File";
    labels[2] = "Cancel";
    tg_gui_sendphoto_box(ui, TG_GUI_PEN_WINDOW, 0, 0, ui->iw, ui->ih);
    /* One info line: which file, how big. The pixel preview comes back once
       the decode path can feed it properly (see ROADMAP: send-photo dialog
       preview); a wrong picture was worse than none. */
    {
        char info[96];
        unsigned long nlen = (unsigned long)strlen(ui->name);

        if (nlen > 34UL) {
            nlen = 34UL;
        }
        memcpy(info, ui->name, nlen);
        sprintf(info + nlen, " (%lu KB)", (ui->bytes + 512UL) / 1024UL);
        tg_gui_sendphoto_text(ui, TG_GUI_PEN_TEXT, 8, 8 + lh,
                              info, (unsigned long)strlen(info));
    }
    tg_gui_sendphoto_text(ui, TG_GUI_PEN_TEXT_DIM, 8,
                          ui->cap_y - 4, "Caption:", 8UL);
    tg_gui_sendphoto_caption_row(ui);
    /* Buttons, right-aligned: [Photo] [File] [Cancel]. */
    {
        int x = ui->iw - 8;

        for (i = 2; i >= 0; --i) {
            int tw = (int)TextLength(rp, (STRPTR)labels[i],
                                     (UWORD)strlen(labels[i]));

            ui->btn_w[i] = tw + 16;
            x -= ui->btn_w[i];
            ui->btn_x[i] = x;
            x -= 6;
        }
        for (i = 0; i < 3; ++i) {
            int fill = (i == 0) ? TG_GUI_PEN_ACCENT : TG_GUI_PEN_SURFACE;
            int ink = (i == 0) ? TG_GUI_PEN_ACCENT_TEXT : TG_GUI_PEN_TEXT;
            int tw = ui->btn_w[i] - 16;
            int ascent = (rp->Font != 0) ? (int)rp->Font->tf_Baseline
                                         : lh - 2;

            tg_gui_sendphoto_box(ui, fill, ui->btn_x[i], ui->btn_y,
                                 ui->btn_w[i], lh + 8);
            SetAPen(rp, ui->main_ctx->pens[ink]);
            SetDrMd(rp, JAM1);
            Move(rp, ui->win->BorderLeft + ui->btn_x[i] + 8 +
                     ((ui->btn_w[i] - 16 - tw) / 2),
                 ui->win->BorderTop + ui->btn_y + 4 + ascent);
            Text(rp, (STRPTR)labels[i], (UWORD)strlen(labels[i]));
        }
    }
}

static int tg_gui_window_send_photo_dialog(tg_gui_state *state,
                                           tg_gui_amiga_ctx *main_ctx,
                                           struct Window *parent,
                                           const char *path, char *caption,
                                           unsigned long caption_size)
{
    tg_gui_sendphoto_ui ui;
    struct TagItem tags[16];
    struct Screen *screen;
    const char *name;
    const char *pp;
    int lh;
    int i;
    int result = 0;
    int done = 0;

    if (state == 0 || main_ctx == 0 || parent == 0 || path == 0 ||
        caption == 0 || caption_size < 2UL) {
        return 0;
    }
    memset(&ui, 0, sizeof(ui));
    ui.main_ctx = main_ctx;
    ui.caption = caption;
    ui.caption_size = caption_size;
    ui.cursor_on = 1;
    /* Desktop behaviour: the composer draft moves into the caption box. */
    caption[0] = '\0';
    ui.caption_len = 0UL;
    for (i = 0; state->input[i] != '\0' &&
                ui.caption_len + 1UL < caption_size; ++i) {
        caption[ui.caption_len++] = state->input[i];
    }
    caption[ui.caption_len] = '\0';
    name = path;
    for (pp = path; *pp != '\0'; ++pp) {
        if (*pp == '/' || *pp == ':') {
            name = pp + 1;
        }
    }
    ui.name = name;
    lh = main_ctx->line_h;
    screen = parent->WScreen;
    /* Only the size is read from the file: the dialog shows name and KB
       while the pixel preview waits for a decode path that deserves it. */
    {
        FILE *f = fopen(path, "rb");

        if (f != 0) {
            long fsz;

            if (fseek(f, 0L, SEEK_END) == 0 && (fsz = ftell(f)) > 0L) {
                ui.bytes = (unsigned long)fsz;
            }
            fclose(f);
        }
    }
    ui.iw = 300;
    if (screen != 0 && ui.iw > (int)screen->Width - 60) {
        ui.iw = (int)screen->Width - 60;
    }
    ui.cap_y = 8 + lh + 6 + lh + 2;
    ui.btn_y = ui.cap_y + lh + 6 + 10;
    ui.ih = ui.btn_y + lh + 8 + 8;
    i = 0;
    tags[i].ti_Tag = WA_Title;
    tags[i++].ti_Data = TG_GUI_TAG("Send photo");
    tags[i].ti_Tag = WA_InnerWidth;
    tags[i++].ti_Data = (ULONG)ui.iw;
    tags[i].ti_Tag = WA_InnerHeight;
    tags[i++].ti_Data = (ULONG)ui.ih;
    tags[i].ti_Tag = WA_Left;
    tags[i++].ti_Data = (ULONG)(parent->LeftEdge + 40);
    tags[i].ti_Tag = WA_Top;
    tags[i++].ti_Data = (ULONG)(parent->TopEdge + 40);
    tags[i].ti_Tag = WA_DragBar;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_DepthGadget;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_CloseGadget;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_Activate;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_SmartRefresh;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_AutoAdjust;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_IDCMP;
    tags[i++].ti_Data = IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY |
                        IDCMP_MOUSEBUTTONS | IDCMP_REFRESHWINDOW |
                        IDCMP_INTUITICKS;
    tags[i].ti_Tag = WA_CustomScreen;
    tags[i++].ti_Data = TG_GUI_TAG(screen);
    tags[i].ti_Tag = TAG_END;
    tags[i++].ti_Data = 0;
    ui.win = OpenWindowTagList(0, tags);
    if (ui.win == 0) {
        return 0;
    }
    if (main_ctx->rport != 0 && main_ctx->rport->Font != 0) {
        SetFont(ui.win->RPort, main_ctx->rport->Font);
    }
    /* Centre the dialog over the main window now that both sizes are real;
       clamped to the screen so a corner-parked window cannot push it off. */
    {
        struct Screen *scr = ui.win->WScreen;
        int nl = (int)parent->LeftEdge +
                 (((int)parent->Width - (int)ui.win->Width) / 2);
        int nt = (int)parent->TopEdge +
                 (((int)parent->Height - (int)ui.win->Height) / 2);

        if (scr != 0) {
            if (nl + (int)ui.win->Width > (int)scr->Width) {
                nl = (int)scr->Width - (int)ui.win->Width;
            }
            if (nt + (int)ui.win->Height > (int)scr->Height) {
                nt = (int)scr->Height - (int)ui.win->Height;
            }
        }
        if (nl < 0) {
            nl = 0;
        }
        if (nt < 0) {
            nt = 0;
        }
        MoveWindow(ui.win, (WORD)(nl - (int)ui.win->LeftEdge),
                   (WORD)(nt - (int)ui.win->TopEdge));
    }
    tg_gui_sendphoto_paint(&ui);
    while (!done) {
        struct IntuiMessage *msg;

        Wait(1UL << ui.win->UserPort->mp_SigBit);
        while ((msg = (struct IntuiMessage *)GetMsg(ui.win->UserPort)) != 0) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code;
            WORD mx = msg->MouseX;
            WORD my = msg->MouseY;

            ReplyMsg((struct Message *)msg);
            if (cls == IDCMP_CLOSEWINDOW) {
                result = 0;
                done = 1;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(ui.win);
                tg_gui_sendphoto_paint(&ui);
                EndRefresh(ui.win, TRUE);
            } else if (cls == IDCMP_INTUITICKS) {
                /* Same cadence as the composer and the search box: the
                   ticks come ~10 a second, the caret flips every fifth. */
                if (++ui.tick_count >= 5) {
                    ui.tick_count = 0;
                    ui.cursor_on = !ui.cursor_on;
                    tg_gui_sendphoto_caption_row(&ui);
                }
            } else if (cls == IDCMP_VANILLAKEY) {
                if (code == 13U) {
                    result = 1;
                    done = 1;
                } else if (code == 27U) {
                    result = 0;
                    done = 1;
                } else if (code == 8U) {
                    if (ui.caption_len > 0UL) {
                        --ui.caption_len;
                        ui.caption[ui.caption_len] = '\0';
                        ui.cursor_on = 1;
                        tg_gui_sendphoto_caption_row(&ui);
                    }
                } else if (code >= 32U && code != 127U &&
                           ui.caption_len + 1UL < ui.caption_size) {
                    ui.caption[ui.caption_len++] = (char)code;
                    ui.caption[ui.caption_len] = '\0';
                    ui.cursor_on = 1;
                    tg_gui_sendphoto_caption_row(&ui);
                }
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                int ix = (int)mx - (int)ui.win->BorderLeft;
                int iy = (int)my - (int)ui.win->BorderTop;

                if (iy >= ui.btn_y && iy < ui.btn_y + lh + 8) {
                    for (i = 0; i < 3; ++i) {
                        if (ix >= ui.btn_x[i] &&
                            ix < ui.btn_x[i] + ui.btn_w[i]) {
                            result = (i == 0) ? 1 : ((i == 1) ? 2 : 0);
                            done = 1;
                        }
                    }
                }
            }
        }
    }
    CloseWindow(ui.win);
    return result;
}

/* Confirm + remove the selected chat from the sidebar, persist it, then land on
   a neighbouring chat (or an empty transcript if the list is now empty). Shared
   by the menu item and the Del key. No-op outside chat mode / with no selection. */
/* Dynamic right-button trap: while the pointer is over a real transcript
   bubble the window claims the right button (WFLG_RMBTRAP -> MENUDOWN comes
   in as a normal MOUSEBUTTONS event for OUR context menu); anywhere else the
   flag is dropped so the right button opens the standard Intuition menu bar.
   Flag poking under Forbid() is the documented classic idiom and works on
   every lane. This replaces IDCMP_MENUVERIFY, whose reply handshake blocked
   input.device system-wide whenever this task was busy in a slow network
   poll -- the "right-click freezes the whole Amiga" report. */
static void tg_gui_amiga_set_rmbtrap(struct Window *win, int on)
{
    if (win == 0) {
        return;
    }
    if (((win->Flags & WFLG_RMBTRAP) != 0UL) == (on != 0)) {
        return; /* already in the wanted state */
    }
    Forbid();
    if (on) {
        win->Flags |= WFLG_RMBTRAP;
    } else {
        win->Flags &= ~(ULONG)WFLG_RMBTRAP;
    }
    Permit();
}

/* Recompute the dynamic right-button trap from the current pointer position:
   claim it (WFLG_RMBTRAP -> our transcript context menu) ONLY over a real
   message bubble, release it everywhere else so the standard Intuition menu
   bar opens. Driven from BOTH the MOUSEMOVE and the ~10/s INTUITICKS handlers.
   The tick pass matters: the OS4 emulated mouse coalesces/drops moves, so a
   move-only update could leave the trap stuck ON after the pointer had already
   left the transcript -- a right-click on the sidebar was then delivered to us
   (not a bubble -> nothing happens, the click is swallowed) and the menu bar
   never appeared. The periodic re-check makes the trap self-heal within
   ~100 ms regardless of how moves are delivered. No-op while our own context
   menu is up (its clicks must keep reaching us). */
static void tg_gui_window_track_rmbtrap(tg_gui_state *state,
                                        const tg_gui_amiga_ctx *ctx,
                                        int mouse_x, int mouse_y)
{
    int hx;
    int hy;
    int hit;
    int over_msg;

    if (state == 0 || ctx == 0 || ctx->window == 0 ||
        state->mode != TG_GUI_MODE_CHAT || state->ctx_visible) {
        return;
    }
    hx = mouse_x - ctx->origin_x;
    hy = mouse_y - ctx->origin_y;
    if (hx < 0 || hy < 0 || hx >= ctx->inner_w || hy >= ctx->inner_h) {
        /* Pointer OUTSIDE our content: release the trap so a right-click
           out there gets Intuition's classic menu bar for this (still
           active) window. The hit test does not bound-check, so without
           this a pointer just past the edge could still map onto a bubble
           and keep the trap armed -- the menu bar then never appeared. */
        tg_gui_amiga_set_rmbtrap(ctx->window, 0);
        return;
    }
    hit = tg_gui_hit_test(state, ctx->inner_w, ctx->inner_h, ctx->line_h,
                          hx, hy);
    over_msg = 0;
    if (hit <= TG_GUI_HIT_MESSAGE_BASE) {
        int mi = hit <= TG_GUI_HIT_PHOTO_BASE
            ? TG_GUI_HIT_PHOTO_BASE - hit
            : TG_GUI_HIT_MESSAGE_BASE - hit;

        over_msg = (mi >= 0 && mi < state->message_count &&
                    !state->messages[mi].is_system &&
                    state->messages[mi].id != 0UL);
    }
    tg_gui_amiga_set_rmbtrap(ctx->window, over_msg);
}

/* Name of the file being uploaded, shown inside the progress line so the
   feedback lasts the whole transfer. Set by both upload entry points (the
   ASL picker and a Workbench drop); cleared when the transfer ends. */
static char tg_gui_xfer_name[24];

static void tg_gui_window_set_transfer_name(const char *name)
{
    unsigned long n = 0UL;

    if (name != 0) {
        while (name[n] != '\0' && n + 1UL < sizeof(tg_gui_xfer_name)) {
            tg_gui_xfer_name[n] = name[n];
            ++n;
        }
    }
    tg_gui_xfer_name[n] = '\0';
}

/* Final status line for a finished non-blocking transfer (trc is the rc from
   tg_gui_session_transfer_end; same codes the blocking calls used). dir: 1 =
   download (`saved` holds the path on 0, the reason otherwise), 2 = upload.
   0.0.8 punto 1b: the old blocking progress hooks (and their UserPort drain)
   are gone -- the event loop itself pumps the transfer now, so input is
   handled where it always is and cancel is a real event, not a drain. */
static void tg_gui_window_transfer_finished(tg_gui_state *state,
                                            tg_gui_backend *backend,
                                            int dir, int trc,
                                            const char *saved)
{
    char line[192];
    int requested_photo;
    int photo_fallback;

    if (saved == 0) {
        saved = "";
    }
    requested_photo = dir == 2 &&
                      tg_gui_session_transfer_requested_photo();
    photo_fallback = requested_photo &&
                     tg_gui_session_transfer_photo_fallback();
    if (dir == 2) {
        if (trc == 0) {
            if (photo_fallback) {
                strcpy(line, "File sent (photo was over 10 MiB)");
            } else if (requested_photo) {
                strcpy(line, "Photo sent");
            } else {
                strcpy(line, "File sent");
            }
        } else if (trc == 2) {
            sprintf(line, "File too big (%lu MiB limit on this build)",
                    tg_gui_session_upload_limit_mib());
        } else if (trc == 3) {
            strcpy(line, requested_photo ? "Could not read that photo"
                                         : "Could not read that file");
        } else if (trc == 5) {
            strcpy(line, "That file is empty (0 bytes)");
        } else if (trc == 6) {
            strcpy(line, requested_photo ? "Photo upload cancelled"
                                         : "Upload cancelled");
        } else if (trc == 7) {
            const char *why = tg_gui_session_last_transfer_error();

            sprintf(line, "Not sent as a photo: %.100s",
                    (why != 0 && why[0] != '\0') ? why
                                                  : "not a valid JPEG or PNG");
        } else {
            const char *why = tg_gui_session_last_transfer_error();

            if (why != 0 && why[0] != '\0') {
                sprintf(line, "Upload failed: %.100s", why);
            } else {
                strcpy(line, "Upload failed");
            }
        }
    } else {
        if (trc == 0) {
            sprintf(line, "Saved to %.160s", saved);
        } else if (trc == 2) {
            strcpy(line, "File is on another server - not supported yet");
        } else if (trc == 3) {
            sprintf(line, "Could not write to %.28s",
                    tg_gui_session_download_dir());
        } else if (trc == 5) {
            strcpy(line, "Download cancelled");
        } else if (trc == 4) {
            if (saved[0] != '\0') {
                sprintf(line, "Transfer failed: %.160s", saved);
            } else {
                strcpy(line, "Transfer failed (server error)");
            }
        } else {
            if (saved[0] != '\0') {
                sprintf(line, "Not found: %.170s", saved);
            } else {
                strcpy(line, "File not found or reference expired");
            }
        }
    }
    tg_gui_window_copy(state->status, sizeof(state->status), line);
    tg_gui_window_paint(state, backend);
}

/* "Download drawer...": ASL drawer requester -> where downloads land from
   now on (0.0.8; a tester on a slow disk wanted them in RAM:). The choice is
   written to data/telegram-downloads.txt, so it survives the next run. */
static void tg_gui_window_pick_download_dir(tg_gui_state *state,
                                            struct Window *win,
                                            tg_gui_backend *backend)
{
    struct FileRequester *req;
    char dir[128];

    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 38L);
    if (AslBase == 0) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "asl.library V38 not found");
        tg_gui_window_paint(state, backend);
        return;
    }
#if defined(__amigaos4__)
    IAsl = (struct AslIFace *)GetInterface(AslBase, "main", 1L, 0);
    if (IAsl == 0) {
        CloseLibrary(AslBase);
        AslBase = 0;
        return;
    }
#endif
    req = (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Window, (unsigned long)win, ASLFR_TitleText,
        (unsigned long)"Where should downloads go?", ASLFR_DrawersOnly, TRUE,
        ASLFR_InitialDrawer,
        (unsigned long)tg_gui_session_download_dir(), TAG_DONE);
    dir[0] = '\0';
    if (req != 0 && AslRequestTags(req, TAG_DONE)) {
        unsigned long n = 0UL;
        const char *p;

        for (p = (const char *)req->fr_Drawer; p != 0 && *p != '\0' &&
             n + 1UL < sizeof(dir); ++p) {
            dir[n++] = *p;
        }
        dir[n] = '\0';
    }
    if (req != 0) {
        FreeAslRequest(req);
    }
#if defined(__amigaos4__)
    DropInterface((struct Interface *)IAsl);
    IAsl = 0;
#endif
    CloseLibrary(AslBase);
    AslBase = 0;
    if (dir[0] == '\0') {
        return; /* cancelled */
    }
    {
        int src = tg_gui_session_set_download_dir(dir);
        char line[80];

        if (src == 0) {
            sprintf(line, "Downloads go to %.30s",
                    tg_gui_session_download_dir());
        } else if (src == 2) {
            sprintf(line, "Downloads: %.24s (this run only)",
                    tg_gui_session_download_dir());
        } else {
            strcpy(line, "That drawer name is not usable");
        }
        tg_gui_window_copy(state->status, sizeof(state->status), line);
        tg_gui_window_paint(state, backend);
    }
}

static int tg_gui_photo_file_exists(const char *path)
{
    FILE *file;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == 0) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int tg_gui_photo_cached_jpeg(char *path, unsigned long path_size,
                                    unsigned long id_hi,
                                    unsigned long id_lo, int large_only)
{
    if (tg_gui_session_photo_cache_path(path, path_size, id_hi, id_lo, 1) ==
            0 &&
        tg_gui_photo_file_exists(path)) {
        return 1;
    }
    if (!large_only &&
        tg_gui_session_photo_cache_path(path, path_size, id_hi, id_lo, 0) ==
            0 &&
        tg_gui_photo_file_exists(path)) {
        return 1;
    }
    path[0] = '\0';
    return 0;
}

/* Copy through a sibling temporary file. When replacing, keep a backup until
   the final rename succeeds so a disk error cannot destroy the old file. */
static int tg_gui_photo_copy_atomic(const char *source,
                                    const char *destination)
{
    FILE *in;
    FILE *out;
    unsigned char *buffer;
    char part[272];
    char backup[272];
    unsigned long destination_len;
    int existed;
    int moved_old;
    int failed;

    if (source == 0 || destination == 0 || source[0] == '\0' ||
        destination[0] == '\0') {
        return 1;
    }
    if (strcmp(source, destination) == 0) {
        return 0;
    }
    destination_len = (unsigned long)strlen(destination);
    if (destination_len + 6UL >= sizeof(part)) {
        return 1;
    }
    sprintf(part, "%s.part", destination);
    sprintf(backup, "%s.bak", destination);
    in = fopen(source, "rb");
    if (in == 0) {
        return 1;
    }
    (void)remove(part);
    out = fopen(part, "wb");
    if (out == 0) {
        fclose(in);
        return 1;
    }
    buffer = (unsigned char *)malloc(8192U);
    failed = buffer == 0;
    while (!failed) {
        size_t got;

        got = fread(buffer, 1, 8192U, in);
        if (got == 0U) {
            if (ferror(in)) {
                failed = 1;
            }
            break;
        }
        if (fwrite(buffer, 1, got, out) != got) {
            failed = 1;
        }
    }
    free(buffer);
    if (fclose(in) != 0) {
        failed = 1;
    }
    if (fclose(out) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)remove(part);
        return 1;
    }
    existed = tg_gui_photo_file_exists(destination);
    moved_old = 0;
    if (existed) {
        (void)remove(backup);
        if (rename(destination, backup) != 0) {
            (void)remove(part);
            return 1;
        }
        moved_old = 1;
    }
    if (rename(part, destination) != 0) {
        if (moved_old) {
            (void)rename(backup, destination);
        }
        (void)remove(part);
        return 1;
    }
    if (moved_old) {
        (void)remove(backup);
    }
    return 0;
}

/* 1 selected, 0 cancelled, -1 requester/path failure. */
static int tg_gui_photo_pick_destination(struct Window *win,
                                         unsigned long id_hi,
                                         unsigned long id_lo,
                                         char *destination,
                                         unsigned long destination_size)
{
    struct FileRequester *req;
    char name[40];
    int selected;
    int result;

    destination[0] = '\0';
    if (tg_gui_photo_default_filename(name, sizeof(name), id_hi, id_lo) != 0) {
        return -1;
    }
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 38L);
    if (AslBase == 0) {
        return -1;
    }
#if defined(__amigaos4__)
    IAsl = (struct AslIFace *)GetInterface(AslBase, "main", 1L, 0);
    if (IAsl == 0) {
        CloseLibrary(AslBase);
        AslBase = 0;
        return -1;
    }
#endif
    req = (struct FileRequester *)AllocAslRequestTags(
        ASL_FileRequest, ASLFR_Window, TG_GUI_TAG(win), ASLFR_TitleText,
        TG_GUI_TAG("Save photo as"), ASLFR_DoSaveMode, TRUE,
        ASLFR_InitialDrawer, TG_GUI_TAG(tg_gui_session_download_dir()),
        ASLFR_InitialFile, TG_GUI_TAG(name),
        /* Same visible pattern as the send side (issue #13): what lands here
           is a JPEG, so the drawer listing shows the photos already saved. */
        ASLFR_DoPatterns, TRUE,
        ASLFR_InitialPattern, TG_GUI_TAG("#?.(jpg|jpeg|png)"), TAG_DONE);
    selected = req != 0 && AslRequestTags(req, TAG_DONE);
    result = 0;
    if (selected) {
        if (tg_gui_photo_build_destination(
                destination, destination_size,
                (const char *)req->fr_Drawer,
                (const char *)req->fr_File) != 0) {
            result = -1;
        } else if (tg_gui_photo_file_exists(destination) &&
                   !tg_gui_photo_save_allowed(
                       1, tg_gui_amiga_confirm_photo_overwrite(win) == 1)) {
            destination[0] = '\0';
            result = 0;
        } else {
            result = 1;
        }
    }
    if (req != 0) {
        FreeAslRequest(req);
    }
#if defined(__amigaos4__)
    DropInterface((struct Interface *)IAsl);
    IAsl = 0;
#endif
    CloseLibrary(AslBase);
    AslBase = 0;
    return result;
}

static void tg_gui_photo_save_status(tg_gui_state *state,
                                     tg_gui_backend *backend,
                                     const char *text)
{
    tg_gui_window_copy(state->status, sizeof(state->status), text);
    tg_gui_window_paint(state, backend);
}

static void tg_gui_photo_save_begin(tg_gui_state *state,
                                    struct Window *win,
                                    tg_gui_backend *backend,
                                    tg_gui_photo_save_job *job,
                                    unsigned long id_hi,
                                    unsigned long id_lo)
{
    char source[64];
    char line[192];
    int picked;

    if (job->pending) {
        tg_gui_photo_save_status(state, backend,
                                 "A photo save is already running");
        return;
    }
    if (tg_gui_session_transfer_busy()) {
        tg_gui_photo_save_status(state, backend,
                                 "A transfer is already running");
        return;
    }
    picked = tg_gui_photo_pick_destination(
        win, id_hi, id_lo, job->destination, sizeof(job->destination));
    if (picked == 0) {
        return;
    }
    if (picked < 0) {
        tg_gui_photo_save_status(state, backend,
                                 "Could not open the save requester");
        return;
    }
    if (tg_gui_photo_cached_jpeg(source, sizeof(source), id_hi, id_lo, 0)) {
        if (tg_gui_photo_copy_atomic(source, job->destination) == 0) {
            sprintf(line, "Saved: %.180s", job->destination);
        } else {
            strcpy(line, "Could not save that photo");
        }
        tg_gui_photo_save_status(state, backend, line);
        return;
    }
    if (tg_gui_session_request_photo_jpeg(id_hi, id_lo, 1) == 0) {
        tg_gui_photo_save_status(state, backend,
                                 "That photo is not available now");
        return;
    }
    job->pending = 1;
    job->last_percent = -1;
    job->id_hi = id_hi;
    job->id_lo = id_lo;
    tg_gui_photo_save_status(state, backend,
                             "Fetching photo... (ESC cancels)");
}

static int tg_gui_photo_save_tick(tg_gui_state *state,
                                  tg_gui_backend *backend,
                                  tg_gui_photo_save_job *job)
{
    char source[64];
    char line[192];
    unsigned long done;
    unsigned long total;
    unsigned long percent;
    int pending;

    if (!job->pending) {
        return 0;
    }
    if (tg_gui_photo_cached_jpeg(source, sizeof(source), job->id_hi,
                                 job->id_lo, 1)) {
        if (tg_gui_photo_copy_atomic(source, job->destination) == 0) {
            sprintf(line, "Saved: %.180s", job->destination);
        } else {
            strcpy(line, "Could not save that photo");
        }
        memset(job, 0, sizeof(*job));
        tg_gui_photo_save_status(state, backend, line);
        return 1;
    }
    pending = tg_gui_session_request_photo_jpeg(job->id_hi, job->id_lo, 1);
    if (pending == 0) {
        memset(job, 0, sizeof(*job));
        tg_gui_photo_save_status(state, backend,
                                 "That photo is not available now");
        return 1;
    }
    done = total = 0UL;
    if (tg_gui_session_photo_fetch_progress(
            job->id_hi, job->id_lo, 1, &done, &total) && total != 0UL) {
        if (total > 42949672UL) {
            percent = done / (total / 100UL);
        } else {
            percent = (done * 100UL) / total;
        }
        if (percent > 100UL) {
            percent = 100UL;
        }
        if ((int)percent != job->last_percent) {
            job->last_percent = (int)percent;
            sprintf(line, "Fetching photo... %lu%% (ESC cancels)", percent);
            tg_gui_photo_save_status(state, backend, line);
            return 1;
        }
    }
    return 0;
}

static void tg_gui_photo_save_cancel(tg_gui_state *state,
                                     tg_gui_backend *backend,
                                     tg_gui_photo_save_job *job)
{
    if (!job->pending) {
        return;
    }
    memset(job, 0, sizeof(*job));
    tg_gui_photo_save_status(state, backend, "Photo save cancelled");
}

static int tg_gui_window_path_is_jpeg(const char *path)
{
    const char *dot;
    const char *p;
    char ext[6];
    int n;

    if (path == 0) {
        return 0;
    }
    dot = 0;
    for (p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == ':') {
            dot = 0;
        } else if (*p == '.') {
            dot = p;
        }
    }
    if (dot == 0) {
        return 0;
    }
    n = 0;
    while (dot[n] != '\0' && n < 5) {
        char c;

        c = dot[n];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        ext[n++] = c;
    }
    ext[n] = '\0';
    return strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0 ||
           strcmp(ext, ".png") == 0;
}

/* ASL file requester -> non-blocking upload on the open chat.
   The requester is synchronous and system-rendered (safe while we are the
   caller); the upload is only ARMED here -- the event loop pumps it one part
   per turn, so the window keeps living during the transfer (0.0.8 1b). */
static void tg_gui_window_send_file_mode(tg_gui_state *state,
                                         struct Window *win,
                                         tg_gui_backend *backend,
                                         int as_photo)
{
    struct FileRequester *req;
    char path[256];
    int rc;

    if (state->mode != TG_GUI_MODE_CHAT || !tg_gui_session_is_open() ||
        state->chat_count <= 0) {
        return;
    }
    if (tg_gui_session_transfer_busy()) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "A transfer is already running");
        tg_gui_window_paint(state, backend);
        return;
    }
    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 38L);
    if (AslBase == 0) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "asl.library V38 not found");
        tg_gui_window_paint(state, backend);
        return;
    }
#if defined(__amigaos4__)
    IAsl = (struct AslIFace *)GetInterface(AslBase, "main", 1L, 0);
    if (IAsl == 0) {
        CloseLibrary(AslBase);
        AslBase = 0;
        return;
    }
#endif
    if (as_photo) {
        req = (struct FileRequester *)AllocAslRequestTags(
            ASL_FileRequest, ASLFR_Window, (unsigned long)win,
            ASLFR_TitleText, (unsigned long)"Send photo to this chat",
            /* InitialPattern rather than AcceptPattern (issue #13,
               javierdlr): the pattern then SHOWS in the requester's gadget,
               so the user can see what is being filtered and widen it. */
            ASLFR_DoPatterns, TRUE, ASLFR_InitialPattern,
            (unsigned long)"#?.(jpg|jpeg|png)", TAG_DONE);
    } else {
        req = (struct FileRequester *)AllocAslRequestTags(
            ASL_FileRequest, ASLFR_Window, (unsigned long)win,
            ASLFR_TitleText, (unsigned long)"Send file to this chat",
            TAG_DONE);
    }
    path[0] = '\0';
    if (req != 0 && AslRequestTags(req, TAG_DONE)) {
        unsigned long n = 0UL;
        const char *p;

        for (p = (const char *)req->fr_Drawer; p != 0 && *p != '\0' &&
             n + 2UL < sizeof(path); ++p) {
            path[n++] = *p;
        }
        /* Join drawer and name the AmigaDOS way: add '/' only when the drawer
           does not already end in ':' or '/'. */
        if (n > 0UL && path[n - 1UL] != ':' && path[n - 1UL] != '/') {
            path[n++] = '/';
        }
        for (p = (const char *)req->fr_File; p != 0 && *p != '\0' &&
             n + 1UL < sizeof(path); ++p) {
            path[n++] = *p;
        }
        path[n] = '\0';
    }
    if (req != 0) {
        FreeAslRequest(req);
    }
#if defined(__amigaos4__)
    DropInterface((struct Interface *)IAsl);
    IAsl = 0;
#endif
    CloseLibrary(AslBase);
    AslBase = 0;
    if (path[0] == '\0') {
        return; /* cancelled */
    }
    if (as_photo) {
        char dcaption[512];
        int drc;

        drc = tg_gui_window_send_photo_dialog(
            state, (tg_gui_amiga_ctx *)backend->context, win, path, dcaption,
            sizeof(dcaption));
        if (drc == 0) {
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Send cancelled");
            tg_gui_window_paint(state, backend);
            return;
        }
        as_photo = (drc == 1);
        rc = as_photo
                 ? tg_gui_session_transfer_start_photo(path, dcaption, stdout)
                 : tg_gui_session_transfer_start_upload(path, dcaption,
                                                        stdout);
        if (rc == 0) {
            /* The draft moved into the dialog and left with the send. */
            state->input[0] = '\0';
            state->input_caret = 0;
        }
    } else {
        rc = tg_gui_session_transfer_start_upload(path, 0, stdout);
    }
    if (rc == 0) {
        const char *pn = path;
        const char *pp;

        for (pp = path; *pp != '\0'; ++pp) {
            if (*pp == '/' || *pp == ':') {
                pn = pp + 1;
            }
        }
        tg_gui_window_set_transfer_name(pn); /* shown in the progress line */
    }
    if (rc != 0) {
        /* Failed before the first part (unreadable, too big, empty...):
           same final lines as ever. rc 6 cannot happen at start. */
        tg_gui_window_transfer_finished(state, backend, 2, rc, "");
        return;
    }
    if (as_photo && tg_gui_session_transfer_photo_fallback()) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "Photo over 10 MiB; sending as file");
    } else {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           as_photo ? "Sending photo... (ESC cancels)"
                                    : "Uploading... (ESC cancels)");
    }
    tg_gui_window_paint(state, backend);
}

static void tg_gui_window_send_file(tg_gui_state *state, struct Window *win,
                                    tg_gui_backend *backend)
{
    tg_gui_window_send_file_mode(state, win, backend, 0);
}

static void tg_gui_window_send_photo(tg_gui_state *state, struct Window *win,
                                     tg_gui_backend *backend)
{
    tg_gui_window_send_file_mode(state, win, backend, 1);
}

static void tg_gui_window_remove_selected(tg_gui_state *state,
                                          struct Window *win,
                                          tg_gui_backend *backend)
{
    int sel;
    unsigned long idx;

    if (state->mode != TG_GUI_MODE_CHAT || !tg_gui_session_is_open()) {
        return;
    }
    sel = state->selected_chat;
    if (sel < 0 || sel >= state->chat_count) {
        return;
    }
    idx = state->chats[sel].index;
    if (idx == 0UL) {
        tg_gui_log("remove: idx 0, ignored");
        return;
    }
    if (idx == TG_GUI_SAVED_PEER_INDEX) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "Saved Messages is always available");
        tg_gui_window_paint(state, backend);
        return;
    }
    tg_gui_log("remove: begin (showing confirm)");
    if (tg_gui_amiga_confirm_remove(win, state->chats[sel].name) != 1) {
        return; /* cancelled */
    }
    if (tg_gui_session_remove_chat(idx, stdout) != 0) {
        /* Silent no-ops read as "stuck": say it and repaint instead. */
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "Could not remove this chat");
        tg_gui_window_paint(state, backend);
        return;
    }
    /* remove_chat reprojected the sidebar (chat_count updated). Open a neighbour
       so the user is never left on the now-gone chat. */
    if (state->chat_count > 0) {
        int nsel = (sel < state->chat_count) ? sel : (state->chat_count - 1);
        tg_gui_window_open_selection(state, nsel, backend);
    } else {
        state->selected_chat = 0;
        state->message_count = 0;
        state->msg_gen++;
        state->title[0] = '\0';
        state->subtitle[0] = '\0';
        tg_gui_window_paint(state, backend);
    }
}

/* Persist the last window GEOMETRY (size + position) to a small file next to
   the binary (Work:TGh, which survives a reboot) so a reopen restores the
   window exactly where the user left it -- the "pin the window" testers asked
   for. Format: "w h x y"; older files hold just "w h" and still load (position
   stays -1 = let Intuition place it), and older binaries reading a new file
   simply ignore the trailing x y. A roomier default on first run than the old
   600x380. */
static void tg_gui_window_load_geom(int *w, int *h, int *x, int *y, int *own)
{
    FILE *f;
    int rw;
    int rh;
    int rx;
    int ry;
    int got;
    char tok[16];

    *w = 820;
    *h = 560;
    *x = -1; /* -1 = no saved position: Intuition picks the spot */
    *y = -1;
    *own = 0; /* opt-in: append " own" to the geometry line for an own screen */
    rw = 0;
    rh = 0;
    rx = -1;
    ry = -1;
    f = fopen("data/telegram-gui-win.txt", "r");
    if (f != 0) {
        got = fscanf(f, "%d %d %d %d", &rw, &rh, &rx, &ry);
        if (got >= 2 && rw >= 320 && rh >= 200 && rw <= 4096 && rh <= 4096) {
            *w = rw;
            *h = rh;
            if (got == 4 && rx >= 0 && ry >= 0 && rx <= 8192 && ry <= 8192) {
                *x = rx;
                *y = ry;
            }
        }
        /* Optional trailing token: "own" = open on an own (private, cloned
           from Workbench) screen. The save path re-writes it, so the user's
           hand-added toggle survives; old binaries just never read this far. */
        if (fscanf(f, "%15s", tok) == 1 &&
            (strcmp(tok, "own") == 0 || strcmp(tok, "OWN") == 0)) {
            *own = 1;
        }
        fclose(f);
    }
}

static void tg_gui_window_save_geom(int w, int h, int x, int y, int own)
{
    FILE *f;

    if (w < 320 || h < 200) {
        return;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    (void)mkdir("data", 0777); /* best-effort; normally the launcher made it */
    f = fopen("data/telegram-gui-win.txt", "w");
    if (f != 0) {
        fprintf(f, "%d %d %d %d%s\n", w, h, x, y, own ? " own" : "");
        fclose(f);
    }
}

/* Recompute the '@' mention popup after a composer edit or caret move: token
   under the caret (tg_gui_mention_token) -> candidate usernames from the open
   group's member cache. Selection resets to the top match on every refresh so
   the popup always answers "what will ENTER insert" at a glance. */
static void tg_gui_window_mention_refresh(tg_gui_state *state)
{
    int start = 0;
    int plen;
    char prefix[TG_GUI_MENTION_LEN];

    state->mention_active = 0;
    state->mention_count = 0;
    state->mention_sel = 0;
    if (!state->composing || state->mode != TG_GUI_MODE_CHAT ||
        !tg_gui_session_is_open()) {
        return;
    }
    plen = tg_gui_mention_token(state->input, state->input_caret, &start);
    if (plen < 0 || plen >= (int)sizeof(prefix)) {
        return;
    }
    memcpy(prefix, state->input + start + 1, (unsigned long)plen);
    prefix[plen] = '\0';
    state->mention_count = tg_gui_session_mention_candidates(
        prefix, &state->mention_items[0][0], TG_GUI_MENTION_LEN,
        TG_GUI_MENTION_MAX, stdout);
    if (state->mention_count > 0) {
        state->mention_active = 1;
        state->mention_start = start;
    }
}

/* Replace the '@'-token under the caret with "@<picked username> " and place
   the caret after the space; closes the popup. Bounded by the input buffer. */
static void tg_gui_window_mention_complete(tg_gui_state *state)
{
    char out[TG_GUI_MSG_TEXT_MAX];
    unsigned long o = 0UL;
    unsigned long caret;
    int i;
    const char *u;

    if (!state->mention_active || state->mention_sel < 0 ||
        state->mention_sel >= state->mention_count) {
        return;
    }
    /* head, up to and including the '@' */
    for (i = 0; i <= state->mention_start && o + 1UL < sizeof(out); ++i) {
        out[o++] = state->input[i];
    }
    u = state->mention_items[state->mention_sel];
    while (*u != '\0' && o + 1UL < sizeof(out)) {
        out[o++] = *u++;
    }
    if (o + 1UL < sizeof(out)) {
        out[o++] = ' ';
    }
    caret = o;
    /* tail: whatever followed the caret */
    u = state->input + state->input_caret;
    while (*u != '\0' && o + 1UL < sizeof(out)) {
        out[o++] = *u++;
    }
    out[o] = '\0';
    tg_gui_window_copy(state->input, sizeof(state->input), out);
    if (caret > (unsigned long)strlen(state->input)) {
        caret = (unsigned long)strlen(state->input);
    }
    state->input_caret = (int)caret;
    state->in_sel_active = 0; /* the rebuilt input invalidates a selection */
    state->mention_active = 0;
    state->mention_count = 0;
}

/* Deletes the composer's selected range, moving the caret to its start.
   1 = a selection was consumed (callers repaint / then insert); 0 = none. */
static int tg_gui_window_input_delete_sel(tg_gui_state *state)
{
    unsigned long n;
    long a;
    long b;
    long lo;
    long hi;

    if (!state->in_sel_active) {
        return 0;
    }
    state->in_sel_active = 0;
    n = (unsigned long)strlen(state->input);
    a = (long)state->in_sel_anchor;
    b = (long)state->input_caret;
    lo = a < b ? a : b;
    hi = a > b ? a : b;
    if (lo < 0) {
        lo = 0;
    }
    if (hi > (long)n) {
        hi = (long)n;
    }
    if (hi <= lo) {
        return 0;
    }
    memmove(&state->input[lo], &state->input[hi],
            n - (unsigned long)hi + 1UL);
    state->input_caret = (int)lo;
    return 1;
}

/* Load the last online search's openable results into the sidebar list so the
   existing renderer + click hit-test present them as a picker. chats[] is
   restored from the cache (tg_gui_session_refresh_chats) on cancel/open. */
static void tg_gui_window_load_search_results(tg_gui_state *state)
{
    int n;
    int k;

    n = tg_gui_session_search_count();
    if (n > TG_GUI_MAX_CHATS) {
        n = TG_GUI_MAX_CHATS;
    }
    for (k = 0; k < n; ++k) {
        const char *nm = tg_gui_session_search_name(k);
        char c;

        tg_gui_window_copy(state->chats[k].name, sizeof(state->chats[k].name),
                           nm);
        state->chats[k].preview[0] = '\0';
        state->chats[k].time[0] = '\0';
        c = nm[0];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        state->chats[k].initials[0] = (c != '\0') ? c : '?';
        state->chats[k].initials[1] = '\0';
        state->chats[k].avatar_color = k % TG_GUI_AVATAR_COLORS;
        state->chats[k].unread = 0;
        state->chats[k].index = (unsigned long)(k + 1);
        state->chats[k].peer_id_hi = 0UL;
        state->chats[k].peer_id_lo = 0UL;
        state->chats[k].flash = 0;
    }
    state->chat_count = n;
    state->selected_chat = 0;
    state->in_search = 1;
}

/* Case-insensitive substring match, ASCII folding only: chat names are
   Latin-1 here and the query comes from the same keyboard. */
static int tg_gui_window_name_matches(const char *name, const char *q)
{
    unsigned long nl = (unsigned long)strlen(name);
    unsigned long ql = (unsigned long)strlen(q);
    unsigned long i;
    unsigned long j;

    if (ql == 0UL) {
        return 1;
    }
    if (ql > nl) {
        return 0;
    }
    for (i = 0UL; i + ql <= nl; ++i) {
        for (j = 0UL; j < ql; ++j) {
            char a = name[i + j];
            char b = q[j];

            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + 32);
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b + 32);
            }
            if (a != b) {
                break;
            }
        }
        if (j == ql) {
            return 1;
        }
    }
    return 0;
}

/* Local-FIRST search (asked twice by the community): every keystroke filters
   the sidebar from the local cache -- instant even on a 68030, no network.
   The final row ("Search Telegram...", index 0 = not a real chat) is the
   explicit gateway to the online contacts.search. */
static void tg_gui_window_filter_chats(tg_gui_state *state,
                                       tg_gui_backend *backend)
{
    int i;
    int k;
    int local_count;

    if (state->search_query[0] == '\0') {
        if (state->in_filter || state->in_search) {
            state->in_filter = 0;
            state->in_search = 0;
            tg_gui_session_refresh_chats();
        }
        /* Empty box: full local list, plus a "Browse all chats..." row ON TOP
           (0.0.8) -- ENTER right away lists every dialog from the server,
           hidden chats included: the way back when the exact name escapes
           you. With a query the online row sits at the BOTTOM as before. */
        if (state->search_active && state->chat_count < TG_GUI_MAX_CHATS) {
            tg_gui_chat *row;
            int m;

            for (m = state->chat_count; m > 0; --m) {
                state->chats[m] = state->chats[m - 1];
            }
            row = &state->chats[0];
            memset(row, 0, sizeof(*row));
            tg_gui_window_copy(row->name, sizeof(row->name),
                               "Browse all chats...");
            row->initials[0] = '>';
            row->initials[1] = '\0';
            row->index = 0UL; /* the marker: not a chat, go online */
            state->chat_count += 1;
            state->selected_chat = 0;
            state->nav_chat = -1;
            state->chat_scroll = 0;
            state->in_filter = 1;
            tg_gui_window_copy(
                state->status, sizeof(state->status),
                state->forward_pick_active
                    ? "Choose a chat - type or ENTER to browse"
                    : "ENTER lists ALL your chats - or type a name");
        }
        tg_gui_window_paint(state, backend);
        return;
    }
    state->in_search = 0;
    /* The filter sees the complete cache, including rows hidden from the normal
       sidebar. Hidden names carry a local marker supplied by the session. */
    tg_gui_session_show_filterable_chats();
    k = 0;
    for (i = 0; i < state->chat_count; ++i) {
        if (tg_gui_window_name_matches(state->chats[i].name,
                                       state->search_query)) {
            if (k != i) {
                state->chats[k] = state->chats[i];
            }
            ++k;
        }
    }
    local_count = k;
    if (k < TG_GUI_MAX_CHATS) {
        tg_gui_chat *row = &state->chats[k];

        memset(row, 0, sizeof(*row));
        tg_gui_window_copy(row->name, sizeof(row->name),
                           "Search Telegram...");
        row->initials[0] = '>';
        row->initials[1] = '\0';
        row->index = 0UL; /* the marker: not a chat, go online */
        ++k;
    }
    state->chat_count = k;
    state->selected_chat = 0; /* first match (or the online row) */
    state->nav_chat = -1;
    state->chat_scroll = 0;
    state->in_filter = 1;
    {
        char st[64];

        if (state->forward_pick_active) {
            sprintf(st, "%d local - ENTER forwards (last: online)",
                    local_count);
        } else {
            sprintf(st, "%d local - arrows + ENTER (last row: online)",
                    local_count);
        }
        tg_gui_window_copy(state->status, sizeof(state->status), st);
    }
    tg_gui_window_paint(state, backend);
}

/* Leave the search UI, restore the real list and open the chat whose
   peer-cache index is `want` (the filtered rows keep their real index). */
static void tg_gui_window_open_by_index(tg_gui_state *state,
                                        tg_gui_backend *backend,
                                        unsigned long want)
{
    int i;

    state->in_filter = 0;
    state->in_search = 0;
    state->search_active = 0;
    state->search_query[0] = '\0';
    state->search_caret = 0;
    (void)tg_gui_session_unhide_chat(want, stdout);
    tg_gui_session_refresh_chats();
    for (i = 0; i < state->chat_count; ++i) {
        if (state->chats[i].index == want) {
            tg_gui_window_open_selection(state, i, backend);
            return;
        }
    }
    tg_gui_window_paint(state, backend); /* gone from cache: plain list */
}

/* Put the real sidebar back after the temporary forward-destination picker.
   The MTProto session and transcript never left the source peer; reselect its
   public cache row because refreshing the projection resets selected_chat. */
static void tg_gui_window_forward_restore(tg_gui_state *state)
{
    unsigned long source_index;
    int i;

    source_index = state->forward_source_index;
    state->forward_pick_active = 0;
    state->forward_message_id = 0UL;
    state->forward_source_index = 0UL;
    state->in_filter = 0;
    state->in_search = 0;
    state->search_active = 0;
    state->search_query[0] = '\0';
    state->search_caret = 0;
    state->nav_chat = -1;
    tg_gui_session_refresh_chats();
    for (i = 0; i < state->chat_count; ++i) {
        if (state->chats[i].index == source_index) {
            state->selected_chat = i;
            state->chat_scroll_to_sel = 1;
            break;
        }
    }
}

/* Complete a destination choice while the source chat remains open. */
static void tg_gui_window_forward_to_index(tg_gui_state *state,
                                           tg_gui_backend *backend,
                                           unsigned long destination_index,
                                           const char *destination_name)
{
    char name[TG_GUI_NAME_MAX];
    unsigned long message_id;
    int rc;

    tg_gui_window_copy(name, sizeof(name), destination_name);
    message_id = state->forward_message_id;
    rc = tg_gui_session_forward(message_id, destination_index, stdout);
    tg_gui_window_forward_restore(state);
    if (rc == 0) {
        sprintf(state->status, "Forwarded to %.32s", name);
    } else {
        const char *why = tg_gui_session_last_action_error();

        if (why != 0 && why[0] != '\0') {
            sprintf(state->status, "Forward failed: %.30s", why);
        } else {
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Could not forward message");
        }
    }
    tg_gui_window_paint(state, backend);
}

static void tg_gui_window_forward_online_result(tg_gui_state *state,
                                                tg_gui_backend *backend,
                                                int result_index)
{
    char name[TG_GUI_NAME_MAX];
    unsigned long destination_index;
    const char *found_name;

    found_name = tg_gui_session_search_name(result_index);
    tg_gui_window_copy(name, sizeof(name), found_name);
    destination_index = 0UL;
    if (tg_gui_session_search_cache_result(result_index, &destination_index,
                                           stdout) != 0 ||
        destination_index == 0UL) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "Could not use that destination");
        tg_gui_window_paint(state, backend);
        return;
    }
    tg_gui_window_forward_to_index(state, backend, destination_index, name);
}

static void tg_gui_window_begin_forward_pick(tg_gui_state *state,
                                             tg_gui_backend *backend,
                                             unsigned long message_id)
{
    unsigned long source_index;

    source_index = tg_gui_session_current_peer_index();
    if (source_index == 0UL || message_id == 0UL) {
        tg_gui_window_copy(state->status, sizeof(state->status),
                           "Could not start forwarding");
        tg_gui_window_paint(state, backend);
        return;
    }
    state->forward_pick_active = 1;
    state->forward_message_id = message_id;
    state->forward_source_index = source_index;
    state->composing = 0;
    state->in_filter = 0;
    state->in_search = 0;
    state->search_active = 1;
    state->search_query[0] = '\0';
    state->search_caret = 0;
    state->nav_chat = -1;
    state->cursor_on = 1;
    tg_gui_session_refresh_chats();
    tg_gui_window_filter_chats(state, backend);
}

/* Run an online search for the current query and show the matches in the sidebar
   as a picker (click one to open). With auto_open_single, a lone match opens
   straight away -- that is what ENTER wants; the as-you-type debounce passes 0 so
   it never opens behind the user's back while typing. Restores the real chat list
   when the query is empty or yields nothing. */
static void tg_gui_window_run_search(tg_gui_state *state, tg_gui_backend *backend,
                                     int auto_open_single)
{
    int cnt;

    state->in_filter = 0; /* the sidebar is about to show ONLINE results */
    /* An empty query is no longer a no-op: it is the browse mode (list ALL
       my dialogs from the server, hidden ones included). */
    tg_gui_window_copy(
        state->status, sizeof(state->status),
        state->forward_pick_active
            ? (state->search_query[0] == '\0' ? "Listing destinations..."
                                               : "Searching destinations...")
            : (state->search_query[0] == '\0' ? "Listing your chats..."
                                               : "Searching Telegram..."));
    tg_gui_window_paint(state, backend);
    cnt = tg_gui_session_search_run(state->search_query, stdout);
    if (cnt == 1 && auto_open_single) {
        if (state->forward_pick_active) {
            tg_gui_window_forward_online_result(state, backend, 0);
            return;
        } else {
            (void)tg_gui_session_search_open_result(0, stdout);
            state->in_search = 0;
            state->search_active = 0;
            state->search_query[0] = '\0';
            tg_gui_window_copy(state->status, sizeof(state->status),
                               "Live - F1-F10 chats, Q quits");
        }
    } else if (cnt >= 1) {
        /* Show the matches in the sidebar; keep the search box focused so ESC
           cancels and more typing refines. The user clicks a result to open it. */
        tg_gui_window_load_search_results(state);
        tg_gui_window_copy(
            state->status, sizeof(state->status),
            state->forward_pick_active
                ? "Choose destination (ESC cancels)"
                : "Pick a result - click it (ESC cancels)");
    } else {
        /* None / error: restore the local filter so the user can edit the query
           and retry instead of being stranded on an empty online picker. */
        state->in_search = 0;
        tg_gui_window_filter_chats(state, backend);
        tg_gui_window_copy(state->status, sizeof(state->status),
                           cnt < 0 ? "Search failed (network?)"
                                   : (state->search_query[0] == '\0'
                                          ? "No chats found"
                                          : "No match - try a name or @username"));
    }
    tg_gui_window_paint(state, backend);
}

static void tg_gui_photo_viewer_fit(unsigned long source_w,
                                    unsigned long source_h,
                                    int max_w, int max_h,
                                    int *out_w, int *out_h)
{
    int cap;
    int w;
    int h;

    if (source_w == 0UL || source_h == 0UL) {
        source_w = 320UL;
        source_h = 240UL;
    }
    if (max_w < 1) {
        max_w = 1;
    }
    if (max_h < 1) {
        max_h = 1;
    }
    cap = max_w > max_h ? max_w : max_h;
    w = h = 1;
    if (tg_image_canonical_size(source_w, source_h, cap, &w, &h) != 0) {
        w = h = 1;
    }
    if (w > max_w) {
        h = (h * max_w) / w;
        w = max_w;
    }
    if (h > max_h) {
        w = (w * max_h) / h;
        h = max_h;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    *out_w = w;
    *out_h = h;
}

static tg_gui_rect tg_gui_photo_viewer_rect(const tg_gui_photo_viewer *viewer)
{
    tg_gui_rect rect;
    unsigned long source_w;
    unsigned long source_h;
    int max_w;
    int max_h;

    rect.x = rect.y = 8;
    rect.w = rect.h = 1;
    if (viewer == 0 || viewer->ctx.window == 0) {
        return rect;
    }
    max_w = viewer->ctx.inner_w - 16;
    max_h = viewer->ctx.inner_h - 16;
    source_w = viewer->slot.w > 0 ? (unsigned long)viewer->slot.w
                                  : viewer->source_w;
    source_h = viewer->slot.h > 0 ? (unsigned long)viewer->slot.h
                                  : viewer->source_h;
    tg_gui_photo_viewer_fit(source_w, source_h, max_w, max_h,
                            &rect.w, &rect.h);
    rect.x = (viewer->ctx.inner_w - rect.w) / 2;
    rect.y = (viewer->ctx.inner_h - rect.h) / 2;
    return rect;
}

static void tg_gui_photo_viewer_render_target(tg_gui_photo_viewer *viewer)
{
    tg_gui_amiga_ctx *ctx;
    tg_gui_rect rect;
    tg_gui_rect clip;

    if (viewer == 0 || viewer->ctx.rport == 0) {
        return;
    }
    ctx = &viewer->ctx;
    SetAPen(ctx->rport, ctx->pens[TG_GUI_PEN_WINDOW]);
    RectFill(ctx->rport, ctx->origin_x, ctx->origin_y,
             ctx->origin_x + ctx->inner_w - 1,
             ctx->origin_y + ctx->inner_h - 1);
    rect = tg_gui_photo_viewer_rect(viewer);
    SetAPen(ctx->rport, ctx->pens[TG_GUI_PEN_SURFACE]);
    RectFill(ctx->rport, ctx->origin_x + rect.x,
             ctx->origin_y + rect.y,
             ctx->origin_x + rect.x + rect.w - 1,
             ctx->origin_y + rect.y + rect.h - 1);
    clip.x = 0;
    clip.y = 0;
    clip.w = ctx->inner_w;
    clip.h = ctx->inner_h;
    (void)tg_gui_photo_draw_slot(ctx, &viewer->slot, rect, clip);
}

static void tg_gui_photo_viewer_paint(tg_gui_photo_viewer *viewer)
{
    tg_gui_amiga_ctx *ctx;
    struct Layer *layer;

    if (viewer == 0 || viewer->ctx.window == 0 || viewer->ctx.rport == 0) {
        return;
    }
    ctx = &viewer->ctx;
    layer = ctx->rport->Layer;
    if (ctx->buf_ok && ctx->buf_bm != 0 &&
        ctx->buf_w == ctx->inner_w && ctx->buf_h == ctx->inner_h) {
        struct RastPort *saved_rport;
        int saved_ox;
        int saved_oy;

        WaitBlit();
        saved_rport = ctx->rport;
        saved_ox = ctx->origin_x;
        saved_oy = ctx->origin_y;
        tg_gui_photo_direct_begin(ctx);
        ctx->rport = &ctx->buf_rp;
        ctx->origin_x = 0;
        ctx->origin_y = 0;
        tg_gui_photo_viewer_render_target(viewer);
        ctx->rport = saved_rport;
        ctx->origin_x = saved_ox;
        ctx->origin_y = saved_oy;
        if (layer != 0) {
            LockLayerRom(layer);
        }
        BltBitMapRastPort(ctx->buf_bm, 0, 0, ctx->rport,
                          ctx->origin_x, ctx->origin_y,
                          ctx->inner_w, ctx->inner_h, 0xC0);
        (void)tg_gui_photo_direct_replay(ctx, 0, 0,
                                         ctx->inner_w, ctx->inner_h);
        if (layer != 0) {
            UnlockLayerRom(layer);
        }
        tg_gui_photo_direct_report(ctx);
    } else {
        if (layer != 0) {
            LockLayerRom(layer);
        }
        tg_gui_photo_viewer_render_target(viewer);
        if (layer != 0) {
            UnlockLayerRom(layer);
        }
        tg_gui_photo_direct_report(ctx);
    }
}

static void tg_gui_photo_viewer_refresh(tg_gui_photo_viewer *viewer)
{
    tg_gui_amiga_ctx *ctx;

    if (viewer == 0 || viewer->ctx.window == 0) {
        return;
    }
    ctx = &viewer->ctx;
    BeginRefresh(ctx->window);
    tg_gui_amiga_measure_geometry(ctx);
    if (ctx->buf_ok && ctx->buf_bm != 0 &&
        ctx->buf_w == ctx->inner_w && ctx->buf_h == ctx->inner_h) {
        BltBitMapRastPort(ctx->buf_bm, 0, 0, ctx->rport,
                          ctx->origin_x, ctx->origin_y,
                          ctx->inner_w, ctx->inner_h, 0xC0);
        (void)tg_gui_photo_direct_replay(ctx, 0, 0,
                                         ctx->inner_w, ctx->inner_h);
    } else {
        tg_gui_photo_viewer_render_target(viewer);
    }
    EndRefresh(ctx->window, TRUE);
    tg_gui_photo_direct_report(ctx);
}

static void tg_gui_photo_viewer_close(tg_gui_photo_viewer *viewer)
{
    struct IntuiMessage *msg;

    if (viewer == 0) {
        return;
    }
    if (viewer->ctx.window != 0) {
        while ((msg = (struct IntuiMessage *)GetMsg(
                    viewer->ctx.window->UserPort)) != 0) {
            ReplyMsg((struct Message *)msg);
        }
        tg_gui_amiga_buffer_free(&viewer->ctx);
        CloseWindow(viewer->ctx.window);
    }
    tg_gui_photo_slot_clear(&viewer->slot);
    memset(viewer, 0, sizeof(*viewer));
}

static int tg_gui_photo_viewer_open_window(tg_gui_photo_viewer *viewer,
                                           const tg_gui_amiga_ctx *main_ctx)
{
    struct TagItem tags[16];
    struct Screen *screen;
    int max_w;
    int max_h;
    int image_w;
    int image_h;
    int inner_w;
    int inner_h;
    int left;
    int top;
    int i;

    if (viewer == 0 || main_ctx == 0 || main_ctx->window == 0) {
        return 1;
    }
    screen = main_ctx->window->WScreen;
    max_w = (int)screen->Width - 48;
    max_h = (int)screen->Height - 64;
    if (max_w > TG_GUI_PHOTO_VIEWER_CANONICAL_CAP) {
        max_w = TG_GUI_PHOTO_VIEWER_CANONICAL_CAP;
    }
    if (max_h > TG_GUI_PHOTO_VIEWER_CANONICAL_CAP) {
        max_h = TG_GUI_PHOTO_VIEWER_CANONICAL_CAP;
    }
    if (max_w < 80) {
        max_w = 80;
    }
    if (max_h < 60) {
        max_h = 60;
    }
    tg_gui_photo_viewer_fit(viewer->source_w, viewer->source_h,
                            max_w, max_h, &image_w, &image_h);
    inner_w = image_w + 16;
    inner_h = image_h + 16;
    if (inner_w < 176) {
        inner_w = 176;
    }
    if (inner_h < 136) {
        inner_h = 136;
    }
    if (inner_w > (int)screen->Width - 16) {
        inner_w = (int)screen->Width - 16;
    }
    if (inner_h > (int)screen->Height - 32) {
        inner_h = (int)screen->Height - 32;
    }
    left = ((int)screen->Width - inner_w) / 2;
    top = ((int)screen->Height - inner_h) / 2;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    i = 0;
    tags[i].ti_Tag = WA_Title;
    tags[i++].ti_Data = TG_GUI_TAG(viewer->title);
    tags[i].ti_Tag = WA_Left;
    tags[i++].ti_Data = (ULONG)left;
    tags[i].ti_Tag = WA_Top;
    tags[i++].ti_Data = (ULONG)top;
    tags[i].ti_Tag = WA_InnerWidth;
    tags[i++].ti_Data = (ULONG)inner_w;
    tags[i].ti_Tag = WA_InnerHeight;
    tags[i++].ti_Data = (ULONG)inner_h;
    tags[i].ti_Tag = WA_DragBar;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_DepthGadget;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_CloseGadget;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_Activate;
    tags[i++].ti_Data = FALSE;
    tags[i].ti_Tag = WA_SmartRefresh;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_NewLookMenus;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_AutoAdjust;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_IDCMP;
    tags[i++].ti_Data = IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY |
                        IDCMP_RAWKEY | IDCMP_REFRESHWINDOW | IDCMP_INTUITICKS;
    tags[i].ti_Tag = WA_CustomScreen;
    tags[i++].ti_Data = TG_GUI_TAG(screen);
    tags[i].ti_Tag = TAG_END;
    tags[i++].ti_Data = 0;

    viewer->ctx.window = OpenWindowTagList(0, tags);
    if (viewer->ctx.window == 0) {
        return 1;
    }
    viewer->ctx.rport = viewer->ctx.window->RPort;
    if (main_ctx->rport != 0 && main_ctx->rport->Font != 0) {
        SetFont(viewer->ctx.rport, main_ctx->rport->Font);
    }
    viewer->ctx.line_h = main_ctx->line_h;
    memcpy(viewer->ctx.pens, main_ctx->pens, sizeof(viewer->ctx.pens));
    memcpy(viewer->ctx.avatar_pens, main_ctx->avatar_pens,
           sizeof(viewer->ctx.avatar_pens));
    viewer->ctx.bitmap_text_compat = main_ctx->bitmap_text_compat;
    viewer->ctx.photo_truecolor = main_ctx->photo_truecolor;
    viewer->ctx.photo_cgx_failed = main_ctx->photo_cgx_failed;
    viewer->ctx.photo_viewer_scope = 1;
    viewer->ctx.photo_dither = main_ctx->photo_dither;
    tg_gui_amiga_measure_geometry(&viewer->ctx);
    tg_gui_amiga_buffer_alloc(&viewer->ctx);
    tg_gui_photo_viewer_paint(viewer);
    ActivateWindow(viewer->ctx.window);
    return 0;
}

static int tg_gui_photo_viewer_prepare_preview(tg_gui_photo_viewer *viewer)
{
    int changed;

    if (viewer == 0 ||
        (viewer->slot.id_hi == 0UL && viewer->slot.id_lo == 0UL)) {
        return 0;
    }
    changed = 0;
    (void)tg_gui_photo_preview_ensure(
        &viewer->ctx, &viewer->slot, viewer->source_w, viewer->source_h,
        TG_GUI_PHOTO_VIEWER_PREVIEW_CAP, 1, &changed);
    return changed;
}

static int tg_gui_photo_viewer_show(tg_gui_photo_viewer *viewer,
                                    const tg_gui_amiga_ctx *main_ctx,
                                    const tg_gui_message *message)
{
    int same;

    if (viewer == 0 || main_ctx == 0 || message == 0 || !message->has_photo ||
        (message->photo_id_hi == 0UL && message->photo_id_lo == 0UL)) {
        return 1;
    }
    same = viewer->slot.id_hi == message->photo_id_hi &&
           viewer->slot.id_lo == message->photo_id_lo;
    if (!same) {
        tg_gui_photo_slot_clear(&viewer->slot);
        viewer->slot.id_hi = message->photo_id_hi;
        viewer->slot.id_lo = message->photo_id_lo;
        viewer->source_w = message->photo_width;
        viewer->source_h = message->photo_height;
    }
    tg_gui_window_copy(viewer->title, sizeof(viewer->title),
                       message->sender[0] != '\0' ? message->sender : "Photo");
    if (tg_gui_session_request_viewer_photo(
            message->photo_id_hi, message->photo_id_lo,
            &viewer->source_w, &viewer->source_h) != 0) {
        return 1;
    }
    /* Pass zero is local and tiny. Prepare it before opening/painting the
       viewer, even when inline photos are disabled and quality work is queued. */
    viewer->ctx.photo_truecolor = main_ctx->photo_truecolor;
    viewer->ctx.photo_dither = main_ctx->photo_dither;
    (void)tg_gui_photo_viewer_prepare_preview(viewer);
    if (viewer->ctx.window == 0 &&
        tg_gui_photo_viewer_open_window(viewer, main_ctx) != 0) {
        return 1;
    }
    SetWindowTitles(viewer->ctx.window, (CONST_STRPTR)viewer->title,
                    (CONST_STRPTR)-1L);
    tg_gui_photo_viewer_paint(viewer);
    WindowToFront(viewer->ctx.window);
    ActivateWindow(viewer->ctx.window);
    return 0;
}

static void tg_gui_photo_viewer_reject(tg_gui_photo_viewer *viewer,
                                       int decode_rc, int bad_cache)
{
    unsigned long id_hi;
    unsigned long id_lo;
    char line[72];

    if (viewer == 0) {
        return;
    }
    id_hi = viewer->slot.id_hi;
    id_lo = viewer->slot.id_lo;
    if (bad_cache) {
        tg_gui_session_photo_decode_failed_variant(id_hi, id_lo, 1);
    }
    if (tg_gui_log_is_enabled()) {
        sprintf(line, "photo: viewer decode fail rc=%d", decode_rc);
        tg_gui_log(line);
    }
    if (viewer->slot.preview_only &&
        (viewer->slot.rgb != 0 || viewer->slot.pen != 0)) {
        tg_image_jpeg_decoder_destroy(viewer->slot.decoder);
        viewer->slot.decoder = 0;
        free(viewer->slot.jpeg);
        viewer->slot.jpeg = 0;
        viewer->slot.jpeg_len = 0UL;
        free(viewer->slot.stage_pen);
        viewer->slot.stage_pen = 0;
        free(viewer->slot.stage_rgb);
        viewer->slot.stage_rgb = 0;
        viewer->slot.stage_ready_rows = 0;
        viewer->slot.stage_pen_rows = 0;
        viewer->slot.decode_done = 0;
        viewer->slot.decode_w = 0;
        viewer->slot.decode_h = 0;
        viewer->slot.state = 1;
        tg_gui_photo_pipeline_release(&viewer->slot);
        return;
    }
    tg_gui_photo_slot_clear(&viewer->slot);
    viewer->slot.id_hi = id_hi;
    viewer->slot.id_lo = id_lo;
    viewer->slot.state = -1;
}

static int tg_gui_photo_viewer_decode_start(tg_gui_photo_viewer *viewer)
{
    FILE *file;
    char path[64];
    long flen;
    unsigned long got;
    int canonical_w;
    int canonical_h;
    int decode_rc;

    if (viewer == 0 || viewer->ctx.window == 0 ||
        (viewer->slot.state != 0 &&
         !(viewer->slot.preview_only && viewer->slot.state == 1)) ||
        (viewer->slot.id_hi == 0UL && viewer->slot.id_lo == 0UL)) {
        return 0;
    }
    canonical_w = canonical_h = 0;
    if (tg_image_canonical_size(viewer->source_w, viewer->source_h,
                                TG_GUI_PHOTO_VIEWER_CANONICAL_CAP,
                                &canonical_w, &canonical_h) != 0) {
        tg_gui_photo_diag("photo: viewer cache geometry failed");
        return 0;
    }
    if (!tg_gui_photo_pipeline_acquire(
            &viewer->slot, TG_GUI_PHOTO_SCOPE_VIEWER)) {
        return 0;
    }
    if (tg_gui_photo_canonical_load_start(
            &viewer->slot, canonical_w, canonical_h, 1)) {
        return 1;
    }
    if (tg_gui_session_photo_cache_path(
            path, sizeof(path), viewer->slot.id_hi,
            viewer->slot.id_lo, 1) != 0) {
        tg_gui_photo_pipeline_release(&viewer->slot);
        return 0;
    }
    file = fopen(path, "rb");
    if (file == 0) {
        int preview_ready;

        /* Keep the foreground request alive if a bounded queue overflow or a
           transient fetch failure removed its previous entry. */
        (void)tg_gui_session_request_viewer_photo(
            viewer->slot.id_hi, viewer->slot.id_lo,
            &viewer->source_w, &viewer->source_h);
        preview_ready = 0;
        if (viewer->slot.state == 0) {
            preview_ready = tg_gui_photo_preview_start(
                &viewer->ctx, &viewer->slot,
                viewer->source_w, viewer->source_h,
                TG_GUI_PHOTO_VIEWER_PREVIEW_CAP, 1);
        }
        tg_gui_photo_pipeline_release(&viewer->slot);
        return preview_ready; /* foreground fetch is still in progress */
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        tg_gui_photo_viewer_reject(viewer, 1, 1);
        return 0;
    }
    flen = ftell(file);
    if (flen <= 0L || (unsigned long)flen > TG_GUI_PHOTO_VIEWER_JPEG_MAX ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        tg_gui_photo_viewer_reject(viewer, 2, 1);
        return 0;
    }
    viewer->slot.jpeg = (unsigned char *)malloc((size_t)flen);
    if (viewer->slot.jpeg == 0) {
        fclose(file);
        tg_gui_photo_viewer_reject(viewer, 4, 0);
        return 0;
    }
    got = (unsigned long)fread(viewer->slot.jpeg, 1, (size_t)flen, file);
    fclose(file);
    if (got != (unsigned long)flen) {
        tg_gui_photo_viewer_reject(viewer, 5, 0);
        return 0;
    }
    viewer->slot.jpeg_len = got;
    viewer->slot.decode_w = canonical_w;
    viewer->slot.decode_h = canonical_h;
    viewer->slot.state = 2;
    if (!tg_gui_photo_begin_quality_sequence(
            &viewer->slot, TG_GUI_PHOTO_VIEWER_DECODE_CAP, &decode_rc)) {
        tg_gui_photo_viewer_reject(viewer, decode_rc, 1);
        return 0;
    }
    if (tg_gui_log_is_enabled()) {
        char line[72];

        sprintf(line, "photo: viewer decode begin at 1/%d",
                1 << viewer->slot.pass_scale);
        tg_gui_log(line);
    }
    return 1;
}

static int tg_gui_photo_viewer_decode_tick(tg_gui_photo_viewer *viewer,
                                           unsigned int mcu_budget,
                                           int pen_row_budget,
                                           unsigned long cache_budget,
                                           int *used_turn,
                                           int *work_kind)
{
    tg_gui_photo_slot *slot;
    tg_gui_photo_slot *owner;
    int decode_rc;
    int changed;
    int started;

    if (used_turn != 0) {
        *used_turn = 0;
    }
    if (work_kind != 0) {
        *work_kind = TG_GUI_PHOTO_WORK_NONE;
    }
    if (viewer == 0 || viewer->ctx.window == 0 ||
        viewer->ctx.photo_resize_active) {
        return 0;
    }
    slot = &viewer->slot;
    started = 0;
    owner = tg_gui_photo_pipeline_owner(TG_GUI_PHOTO_SCOPE_VIEWER);
    if (tg_gui_photo_decode_pipeline.owner != 0 && owner != slot) {
        return 0;
    }
    if (owner == 0) {
        if (slot->state == 2) {
            if (!tg_gui_photo_pipeline_acquire(
                    slot, TG_GUI_PHOTO_SCOPE_VIEWER)) {
                return 0;
            }
        } else if (slot->state == 0 ||
                   (slot->preview_only && slot->state == 1)) {
            if (!tg_gui_photo_viewer_decode_start(viewer)) {
                return 0;
            }
            started = 1;
        }
        owner = tg_gui_photo_pipeline_owner(TG_GUI_PHOTO_SCOPE_VIEWER);
    }
    if (owner != slot || slot->state != 2 ||
        !tg_gui_photo_pipeline_owns(slot, TG_GUI_PHOTO_SCOPE_VIEWER)) {
        return 0;
    }
    if (used_turn != 0) {
        *used_turn = 1;
    }
    if (started) {
        if (work_kind != 0) {
            *work_kind = TG_GUI_PHOTO_WORK_CACHE;
        }
        return 0;
    }
    changed = tg_gui_photo_quality_tick(
        &viewer->ctx, slot, mcu_budget, pen_row_budget, cache_budget,
        TG_GUI_PHOTO_VIEWER_DECODE_CAP, 1, &decode_rc, work_kind);
    if (changed < 0) {
        tg_gui_photo_viewer_reject(viewer, decode_rc, 1);
        return 1;
    }
    return changed;
}

static void tg_gui_photo_viewer_drain(tg_gui_photo_viewer *viewer,
                                      int *photo_tick,
                                      int *interactive_event,
                                      int *save_requested)
{
    struct IntuiMessage *msg;
    int close_requested;

    if (viewer == 0 || viewer->ctx.window == 0) {
        return;
    }
    close_requested = 0;
    while ((msg = (struct IntuiMessage *)GetMsg(
                viewer->ctx.window->UserPort)) != 0) {
        ULONG msg_class;
        UWORD msg_code;

        msg_class = msg->Class;
        msg_code = msg->Code;
        ReplyMsg((struct Message *)msg);
        if (msg_class == IDCMP_INTUITICKS) {
            if (photo_tick != 0) {
                *photo_tick = 1;
            }
        } else if (interactive_event != 0) {
            *interactive_event = 1;
        }
        if (msg_class == IDCMP_CLOSEWINDOW ||
            (msg_class == IDCMP_VANILLAKEY && msg_code == 27U) ||
            (msg_class == IDCMP_RAWKEY && msg_code == 0x45U)) {
            close_requested = 1;
        } else if (msg_class == IDCMP_VANILLAKEY &&
                   (msg_code == 's' || msg_code == 'S')) {
            if (save_requested != 0) {
                *save_requested = 1;
            }
        } else if (msg_class == IDCMP_REFRESHWINDOW) {
            tg_gui_photo_viewer_refresh(viewer);
        }
    }
    if (close_requested) {
        tg_gui_photo_viewer_close(viewer);
    }
}

static int tg_gui_window_user_events_pending(
    const tg_gui_amiga_ctx *ctx, const tg_gui_photo_viewer *viewer)
{
    ULONG mask;

    if (ctx == 0 || ctx->window == 0 || ctx->window->UserPort == 0) {
        return 0;
    }
    mask = 1UL << ctx->window->UserPort->mp_SigBit;
    if (viewer != 0 && viewer->ctx.window != 0 &&
        viewer->ctx.window->UserPort != 0) {
        mask |= 1UL << viewer->ctx.window->UserPort->mp_SigBit;
    }
    return (SetSignal(0UL, 0UL) & mask) != 0UL;
}

static int tg_gui_run_window_once(tg_gui_state *state)
{
    tg_gui_amiga_ctx ctx;
    tg_gui_photo_viewer viewer;
    tg_gui_photo_save_job photo_save;
    tg_gui_backend backend;
    int init_w;
    int init_h;
    int init_x;
    int init_y;
    int init_own;
    int want_own; /* own-screen preference, toggled live by the menu */
    struct Screen *own_scr;
    struct TagItem tags[24];
    struct ColorMap *cmap;
    struct TextFont *font;
    APTR vi;
    struct Menu *menu;
    unsigned long mem_before;
    unsigned long mem_after;
    unsigned long footprint;
    int i;
    int done;
    struct MsgPort *timer_port = 0; /* live-reception heartbeat (timer.device) */
    tg_gui_timereq *timer_req = 0;
    int timer_ok = 0;
    int timer_pending = 0;
    int timer_fast = 0;
    int caret_ticks;
    time_t xfer_mark;          /* rate window start (transfer progress) */
    unsigned long xfer_bytes;  /* bytes at xfer_mark */
    unsigned long xfer_kbs;    /* last computed KB/s, 0 = not yet known */
    int older_exhausted;   /* load-older confirmed the chat start; re-armed off-top / on open */
    int older_cooldown;    /* wakes to wait before another load-older (slow-link breather) */
    int prev_selected;     /* last selected_chat: a change means a (re)opened chat -> re-arm */
    /* Double-click reply: a plain click selects/highlights a bubble; a second
       click on the SAME bubble within the system double-click interval opens
       the reply. dbl_last_id is the previous click's target MESSAGE ID (not a
       row index -- indexes are reused when the chat changes) plus its press
       time; dbl_press_* carries this click's press time from SELECTDOWN to the
       SELECTUP where the gesture is decided. */
    unsigned long dbl_last_secs = 0;
    unsigned long dbl_last_micros = 0;
    unsigned long dbl_last_id = 0;
    unsigned long dbl_press_secs = 0;
    unsigned long dbl_press_micros = 0;
    /* When a click on a SEARCH RESULT opens a chat, the sidebar is replaced
       by the real list underneath: the second click of a natural double
       click then lands on a different row and opens the chat below it (a
       tester hit this every time on OS4). These carry the opening click's
       time so the very next click on the sidebar can be ignored if it falls
       within the system double-click interval. */
    unsigned long picked_secs = 0;
    unsigned long picked_micros = 0;
    unsigned long watch_seconds;
    unsigned long watch_boot_seconds;
    unsigned long watch_boot_grace;
    unsigned long effective_watch;
    time_t session_boot;
    time_t last_session_poll;
    time_t last_receive_drain;
    time_t last_key_time;
    int resize_pending;
    int resize_settle_ticks;
    int photo_defer_ticks;
    int photo_stall_reason;
    int photo_fast_wake;
    tg_gui_photo_pace photo_decode_pace;
    tg_gui_photo_pace photo_cache_pace;
    tg_gui_photo_pace photo_replay_pace;

    if (state == 0) {
        return 2;
    }

    if (!tg_gui_amiga_open_core_libs()) {
        puts("gui window: cannot open core GUI libraries");
        return 2;
    }

    memset(&ctx, 0, sizeof(ctx));
    memset(&viewer, 0, sizeof(viewer));
    memset(&photo_save, 0, sizeof(photo_save));
    ctx.photo_dither = state->photo_dither;
    own_scr = 0;
    tg_gui_emoji_recent_load(state);
    tg_gui_window_load_geom(&init_w, &init_h, &init_x, &init_y, &init_own);
    want_own = init_own;
    /* Own-screen mode (opt-in " own" token in telegram-gui-win.txt): a PRIVATE
       screen cloned from Workbench (SA_LikeWorkbench, V39 on every lane). The
       testers' "move to another page" gadget is MUI-only, so an app screen is
       the one way to give them a dedicated page. Key tags: SA_SharePens=TRUE
       (without it a paletted OS3 clone holds pens exclusive and every
       ObtainBestPen fails), SA_Pens={~0} (full new-look with the user's pen
       preferences), SA_Behind (the screen surfaces only after the first locked
       paint -- same anti-race discipline as WA_Activate=FALSE). Any open
       failure (no chip RAM on a stock A1200, unknown mode...) falls back to
       the normal Workbench-window path -- never a hard failure. */
    if (init_own) {
        ULONG oserr = 0;
        static UWORD own_pens[] = { (UWORD)~0 };
        struct TagItem stags[9];
        int s = 0;

        stags[s].ti_Tag = SA_LikeWorkbench;
        stags[s++].ti_Data = TRUE;
        stags[s].ti_Tag = SA_Title;
        stags[s++].ti_Data = TG_GUI_TAG("Telegram Amiga");
        stags[s].ti_Tag = SA_Pens;
        stags[s++].ti_Data = TG_GUI_TAG(own_pens);
        stags[s].ti_Tag = SA_SharePens;
        stags[s++].ti_Data = TRUE;
        stags[s].ti_Tag = SA_SysFont;
        stags[s++].ti_Data = 1;
        stags[s].ti_Tag = SA_ShowTitle;
        stags[s++].ti_Data = TRUE;
        stags[s].ti_Tag = SA_Behind;
        stags[s++].ti_Data = TRUE;
        stags[s].ti_Tag = SA_ErrorCode;
        stags[s++].ti_Data = TG_GUI_TAG(&oserr);
        stags[s].ti_Tag = TAG_END;
        stags[s++].ti_Data = 0;
        own_scr = OpenScreenTagList(0, stags);
        if (own_scr == 0) {
            printf("gui window: own screen failed (err %lu) - using default\n",
                   (unsigned long)oserr);
        } else {
            /* The saved geometry may exceed the clone (the Workbench mode may
               have shrunk since the save): clamp so the window open cannot
               fail for a stale size. */
            if (init_w > (int)own_scr->Width - 8) {
                init_w = (int)own_scr->Width - 8;
            }
            if (init_h > (int)own_scr->Height - 32) {
                init_h = (int)own_scr->Height - 32;
            }
        }
    }
    i = 0;
    /* Saved position first (when any): if this exact spot no longer fits the
       screen, OpenWindowTagList FAILS -- the open call below retries once with
       these two tags neutralised, falling back to Intuition's own placement. */
    if (init_x >= 0 && init_y >= 0) {
        tags[i].ti_Tag = WA_Left;
        tags[i++].ti_Data = (ULONG)init_x;
        tags[i].ti_Tag = WA_Top;
        tags[i++].ti_Data = (ULONG)init_y;
    }
    tags[i].ti_Tag = WA_Title;
    tags[i++].ti_Data = TG_GUI_TAG("Telegram Amiga - GUI");
    tags[i].ti_Tag = WA_InnerWidth;
    tags[i++].ti_Data = (ULONG)init_w;
    tags[i].ti_Tag = WA_InnerHeight;
    tags[i++].ti_Data = (ULONG)init_h;
    tags[i].ti_Tag = WA_DragBar;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_DepthGadget;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_CloseGadget;
    tags[i++].ti_Data = TRUE;
#if defined(__amigaos4__)
    /* OS4 titlebar iconify gadget (like OWB's): a click sends IDCMP_CLOSEWINDOW
       with Code == 1, which we route to the same AppIcon park as the menu. */
    tags[i].ti_Tag = WA_IconifyGadget;
    tags[i++].ti_Data = TRUE;
#endif
    tags[i].ti_Tag = WA_SizeGadget;
    tags[i++].ti_Data = TRUE;
    /* Open INACTIVE: activation is what makes the input.device/intuition task
       build this window's layer/ClipRect list, and an immediate paint that races
       that build is what freezes MorphOS inside layers3d. We paint the first
       frame under LockLayerRom() and only then ActivateWindow() (see below). */
    tags[i].ti_Tag = WA_Activate;
    tags[i++].ti_Data = FALSE;
    tags[i].ti_Tag = WA_SmartRefresh;
    tags[i++].ti_Data = TRUE;
    /* New-look menus (white, system pens): without this tag the menubar
       renders in the 1.x old look, white-on-black (a 0.0.7 field report). */
    tags[i].ti_Tag = WA_NewLookMenus;
    tags[i++].ti_Data = TRUE;
    tags[i].ti_Tag = WA_MinWidth;
    tags[i++].ti_Data = 320;
    tags[i].ti_Tag = WA_MinHeight;
    tags[i++].ti_Data = 200;
    tags[i].ti_Tag = WA_MaxWidth;
    tags[i++].ti_Data = 0xffff;
    tags[i].ti_Tag = WA_MaxHeight;
    tags[i++].ti_Data = 0xffff;
    tags[i].ti_Tag = WA_IDCMP;
    tags[i++].ti_Data = IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_RAWKEY |
                        IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW | IDCMP_INTUITICKS |
                        IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_MENUPICK
                        /* NO IDCMP_MENUVERIFY, ever: input.device blocks the
                           WHOLE SYSTEM until the verify is replied, and this
                           task can be tens of seconds deep in a network poll
                           (heavy channels) -- a right-click then froze all of
                           OS3. The context menu uses a dynamic WFLG_RMBTRAP
                           instead (see the MOUSEMOVE handler). */
#if defined(__amigaos4__)
                        /* OS4: the wheel arrives as IDCMP_EXTENDEDMOUSE -- the only
                           form QEMU's emulated mouse emits. Real hardware and the
                           other platforms also send the NewMouse RAWKEY 0x7A/0x7B. */
                        | IDCMP_EXTENDEDMOUSE
#endif
                        ;
    /* MOUSEMOVE is only delivered with REPORTMOUSE (or a follow-mouse gadget),
       so the scrollbar knob-drag needs this. The handler ignores moves unless a
       knob is grabbed, so the extra reports cost nothing when idle. */
    tags[i].ti_Tag = WA_ReportMouse;
    tags[i++].ti_Data = TRUE;
    if (own_scr != 0) {
        /* OWNER window on our private screen (WA_CustomScreen, not a visitor):
           CloseScreen at teardown stays deterministically under our control. */
        tags[i].ti_Tag = WA_CustomScreen;
        tags[i++].ti_Data = TG_GUI_TAG(own_scr);
    }
    tags[i].ti_Tag = TAG_END;
    tags[i++].ti_Data = 0;

    /* Sample free memory now -- after the libraries are open -- so the
       footprint delta isolates the window, RastPort and pens, not the resident
       cost of opening intuition/graphics. */
    mem_before = (unsigned long)AvailMem(MEMF_ANY);

    ctx.window = OpenWindowTagList(0, tags);
    if (ctx.window == 0 && init_x >= 0 && init_y >= 0) {
        /* The remembered position no longer fits (smaller screen/mode since the
           save). Neutralise WA_Left/WA_Top -- they are tags[0]/tags[1] when a
           position was loaded -- and let Intuition place the window instead. */
        tags[0].ti_Tag = TAG_IGNORE;
        tags[1].ti_Tag = TAG_IGNORE;
        ctx.window = OpenWindowTagList(0, tags);
    }
    if (ctx.window == 0 && own_scr != 0) {
        /* Still no window on the own screen: give the screen up and retry on
           the default public screen (the WA_CustomScreen pair is the one right
           before TAG_END). Degraded > dead. */
        tags[i - 2].ti_Tag = TAG_IGNORE;
        CloseScreen(own_scr);
        own_scr = 0;
        ctx.window = OpenWindowTagList(0, tags);
    }
    if (ctx.window == 0) {
        puts("gui window: cannot open window");
        tg_gui_amiga_close_core_libs();
        return 2;
    }

    ctx.rport = ctx.window->RPort;
    tg_gui_window_resolve_inline_default(state, ctx.window);
    if (own_scr != 0 && own_scr->RastPort.Font != 0) {
        /* SA_SysFont sets the SCREEN font, but a window RastPort still comes up
           with the fixed-width DefaultFont (autodoc caveat) -- adopt the screen
           font so text metrics match the Workbench-window mode exactly. */
        SetFont(ctx.rport, own_scr->RastPort.Font);
    }
    font = ctx.rport->Font;
    ctx.line_h = (font != 0 ? (int)font->tf_YSize : 8) + 2;
    ctx.bitmap_text_compat = tg_gui_amiga_afa_text_compat();
    if (ctx.bitmap_text_compat) {
        tg_gui_log("window: AfA bitmap-text compatibility active");
    }
    cmap = ctx.window->WScreen->ViewPort.ColorMap;
    tg_gui_amiga_obtain_pens(&ctx, cmap);
    tg_gui_av_reset();          /* pens are per-screen: drop stale avatar pens */
    tg_gui_av_cmap = cmap;      /* arms the real-avatar path */
    {
        /* Avatar pen profile from THIS screen's depth: truecolor RTG affords
           the rich budget even on m68k (Vampire); paletted stays lean. */
        ULONG av_depth = GetBitMapAttr(ctx.window->WScreen->RastPort.BitMap,
                                       BMA_DEPTH);

        tg_gui_av_rich = av_depth > 8UL;
        tg_gui_av_pool_cap = tg_gui_av_rich ? TG_GUI_AV_POOL_MAX : 48;
        tg_gui_av_share_d = tg_gui_av_rich ? 48L : 192L;
        ctx.photo_truecolor =
            tg_gui_av_rich && tg_gui_amiga_open_cybergraphics();
        if (ctx.photo_truecolor) {
            tg_gui_log("window: RGB888 inline-photo replay active");
        }
    }
    tg_gui_amiga_measure_geometry(&ctx);
    tg_gui_amiga_buffer_alloc(&ctx); /* off-screen double-buffer (flicker-free) */
    /* 0.0.8 punto 1e: an icon dropped on the window uploads the file to the
       open chat. Best-effort: a failed arm just means no drag-and-drop. */
    tg_gui_log(tg_platform_gui_drop_arm(ctx.window) == 0
                   ? "window: drag-and-drop armed"
                   : "window: drag-and-drop NOT armed");

    /* Right-button menu via GadTools (optional: a missing gadtools.library or a
       layout failure just leaves the window menu-less). */
    vi = 0;
    menu = 0;
    if (GadToolsBase != 0) {
        vi = GetVisualInfoA(ctx.window->WScreen, 0);
        if (vi != 0) {
            /* GTMN_NewLookMenus matches WA_NewLookMenus above: both are
               needed or the items keep the old black look on OS3. */
            struct TagItem lmtags[2];

            lmtags[0].ti_Tag = GTMN_NewLookMenus;
            lmtags[0].ti_Data = TRUE;
            lmtags[1].ti_Tag = TAG_DONE;
            lmtags[1].ti_Data = 0;
            menu = CreateMenusA(tg_gui_newmenu, 0);
            if (menu != 0 && LayoutMenusA(menu, vi, lmtags)) {
                /* Reflect the current own-screen mode in the toggle's tick. */
                if (want_own) {
                    struct MenuItem *it2 = tg_gui_menu_find_userdata(
                        menu, (APTR)TG_MENU_OWNSCREEN);

                    if (it2 != 0) {
                        it2->Flags |= CHECKED;
                    }
                }
                if (state->inline_photos) {
                    struct MenuItem *it2 = tg_gui_menu_find_userdata(
                        menu, (APTR)TG_MENU_INLINEPHOTOS);

                    if (it2 != 0) {
                        it2->Flags |= CHECKED;
                    }
                }
                tg_gui_menu_set_photo_dither(menu, state->photo_dither);
                tg_gui_menu_set_photo_cache_limit(
                    menu, state->photo_cache_limit_mb);
                SetMenuStrip(ctx.window, menu);
            }
        }
    }

    tg_gui_log("window: setup done");
    backend.context = &ctx;
    backend.width = tg_gui_amiga_width;
    backend.height = tg_gui_amiga_height;
    backend.line_height = tg_gui_amiga_line_height;
    backend.font_ascent = tg_gui_amiga_font_ascent;
    backend.text_width = tg_gui_amiga_text_width;
    backend.fill_rect = tg_gui_amiga_fill_rect;
    backend.avatar_fill = tg_gui_amiga_avatar_fill;
    backend.avatar_image = tg_gui_amiga_avatar_image;
    backend.glyph_image = tg_gui_amiga_glyph_image;
    backend.photo_image = tg_gui_amiga_photo_image;
    backend.draw_text = tg_gui_amiga_draw_text;
    backend.set_style = tg_gui_amiga_set_style;
    backend.fill_pill = tg_gui_amiga_fill_pill;
    backend.round_bg = TG_GUI_PEN_WINDOW;

    mem_after = (unsigned long)AvailMem(MEMF_ANY);
    footprint = (mem_before > mem_after) ? (mem_before - mem_after) : 0UL;

    /* Paint the initial content ONCE, under the layer lock. The window was opened
       INACTIVE (WA_Activate=FALSE) precisely so this first paint does not race the
       input.device/intuition task that builds the layer/ClipRect list at
       activation: a former 60x "benchmark" burst here mutated the same cliprect
       chain that the input task was building, which then walked a corrupted node
       and wrote through it inside layers3d, freezing the whole machine (DSI store
       to protected memory -- the "lists a few chats then the system freezes"
       report). OpenWindowTagList only guarantees a non-NULL Window; it does NOT
       guarantee when the layer is safe to draw, so tg_gui_window_paint() takes
       LockLayerRom() -- the one layers primitive sanctioned for Intuition windows,
       which blocks Intuition's window machinery from touching this layer while we
       render -- around every direct paint. We activate the window only AFTER this
       first locked paint settles (ActivateWindow must NOT be called while a layer
       is locked, so it goes here, after the wrapper has unlocked). Later repaints
       are IDCMP-driven and the IDCMP_REFRESHWINDOW path is bracketed by
       BeginRefresh/EndRefresh, which carries its own layer lock. */
    tg_gui_window_paint(state, &backend);
    tg_gui_log("window: first paint done");
    if (own_scr != 0) {
        /* The screen opened BEHIND (SA_Behind) so nobody saw the pre-paint
           window; surface it only now that the first locked paint settled --
           and never while a layer lock is held. */
        ScreenToFront(own_scr);
    }
    tg_gui_log("window: activating");
    ActivateWindow(ctx.window);
    printf("gui window: open %dx%d, font %dpx, %lu pens; window footprint "
           "~%lu KB\n",
           ctx.inner_w, ctx.inner_h, ctx.line_h,
           (unsigned long)(TG_GUI_PEN_COUNT + TG_GUI_AVATAR_COLORS),
           footprint / 1024UL);
    fflush(stdout);

    puts("gui window: close gadget or Q to quit.");
    fflush(stdout);
    tg_gui_log("window: opened");

    /* When a live session is attached (--gui-live), IDCMP_INTUITICKS (~10/s
       while the window is active) drives the network poll: throttle the actual
       tick to the per-platform watch interval so a slow link is not hammered
       (MorphOS especially), and coalesce into a single repaint per wake-up. The
       tick is a no-op when no session is open (demo/--gui-chats). */
#if defined(__MORPHOS__) || defined(__MORPHOS)
    /* ADAPTIVE RAMP on MorphOS: slow for the first WATCH_BOOT_GRACE seconds after
       the window opens, then faster steady-state. The 2026-06-20 boot freeze at a
       flat 1s was the PPC STACK OVERFLOW (Background CLI hit 32756/32756 bytes of
       the libnix default ~32KB task stack) -- now CURED by `__stack = 1MB` in
       platforms/morphos/tg_platform_morphos.c. With the stack fixed, the only
       remaining caution is the startup network burst (session open = DH + first
       connect + push-backlog drain): keep the FIRST few seconds at the proven 6s
       so that settles undisturbed, then drop to 3s steady-state -- halving the
       reception latency (the poll interval IS the latency for the open chat) while
       staying boot-safe. The per-tick getDifference is throttled to a backstop;
       pushes carry the live cross-chat stream. */
    watch_seconds = 3UL;
    watch_boot_seconds = 6UL;
    watch_boot_grace = 12UL;
#else
    watch_seconds = 2UL;
    watch_boot_seconds = 2UL;
    watch_boot_grace = 0UL;
#endif
    session_boot = time(0);
    last_session_poll = time(0);
    last_receive_drain = time(0);
    last_key_time = time(0);
    done = 0;
    state->composing = 0;
    state->nav_chat = -1;   /* no arrow-key focus yet (0 would tint row 0) */
    state->in_filter = 0;
    state->forward_pick_active = 0;
    state->forward_message_id = 0UL;
    state->forward_source_index = 0UL;
    state->history_count = 0;
    state->history_pos = -1;
    state->history_draft[0] = '\0';
    state->chat_scroll = 0;
    state->transcript_scroll = 0;
    state->sb_drag = 0;
    state->drag_src = -1; /* no row-reorder drag armed */
    state->drag_active = 0;
    /* A login screen shows its caret from the first frame. */
    state->cursor_on = (state->mode != TG_GUI_MODE_CHAT) ? 1 : 0;
    caret_ticks = 0;
    xfer_mark = (time_t)0;
    xfer_bytes = 0UL;
    xfer_kbs = 0UL;
    older_exhausted = 0;
    older_cooldown = 0;
    prev_selected = state->selected_chat;
    resize_pending = 0;
    resize_settle_ticks = 0;
    photo_defer_ticks = 0;
    photo_stall_reason = TG_GUI_PHOTO_STALL_NONE;
    photo_fast_wake = 0;
    tg_gui_photo_pace_init(&photo_decode_pace,
                           TG_GUI_PHOTO_WORK_MIN,
                           TG_GUI_PHOTO_WORK_INITIAL,
                           TG_GUI_PHOTO_WORK_MAX,
                           TG_GUI_PHOTO_PACE_TARGET_MS);
    tg_gui_photo_pace_init(&photo_cache_pace,
                           TG_GUI_PHOTO_CACHE_MIN,
                           TG_GUI_PHOTO_CACHE_INITIAL,
                           TG_GUI_PHOTO_CACHE_MAX,
                           TG_GUI_PHOTO_PACE_TARGET_MS);
    tg_gui_photo_pace_init(&photo_replay_pace,
                           TG_GUI_PHOTO_WORK_MIN,
                           TG_GUI_PHOTO_WORK_INITIAL,
                           TG_GUI_PHOTO_WORK_MAX,
                           TG_GUI_PHOTO_PACE_TARGET_MS);
    /* Live-reception heartbeat. INTUITICKS are delivered ONLY to the ACTIVE
       window, so with the window deactivated the loop slept in Wait() and the
       network poll never ran -- incoming messages stalled until the user came
       back and clicked around (A4000/Roadshow report). A timer.device VBLANK
       request on the same Wait() wakes the loop every TG_GUI_HEARTBEAT_SECS
       regardless of activation; the poll keeps its own cadence/composing
       guards, so an ACTIVE window behaves exactly as before. If any setup step
       fails we simply keep the old active-only behavior. */
    timer_port = CreateMsgPort();
    if (timer_port != 0) {
        timer_req = (tg_gui_timereq *)CreateIORequest(timer_port,
                                                      sizeof(tg_gui_timereq));
    }
    if (timer_req != 0 &&
        OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)timer_req, 0) == 0) {
        timer_ok = 1;
        tg_gui_timer_arm(timer_req, 0);
        timer_pending = 1;
    }
    while (!done) {
        struct IntuiMessage *msg;
        int session_dirty;
        int scroll_dirty;
        int want_older;     /* a transcript scroll-up reached the top this wake */
        int reveal_older;   /* a fits-window load happened: scroll to show it */
        int photo_tick;
        int interactive_event;
        int photo_resume_turn;
        int photo_background_turn;
        int viewer_dirty;
        int viewer_save_requested;
        ULONG wake_signals;

        session_dirty = 0;
        scroll_dirty = 0;
        want_older = 0;
        reveal_older = 0;
        photo_tick = 0;
        interactive_event = 0;
        photo_resume_turn = 0;
        photo_background_turn = 0;
        viewer_dirty = 0;
        viewer_save_requested = 0;
        wake_signals = 0UL;
        if (older_cooldown > 0) {
            older_cooldown -= 1;
        }
        {
            ULONG wait_mask = 1UL << ctx.window->UserPort->mp_SigBit;

            if (timer_ok) {
                wait_mask |= 1UL << timer_port->mp_SigBit;
            }
            if (viewer.ctx.window != 0 &&
                viewer.ctx.window->UserPort != 0) {
                wait_mask |= 1UL << viewer.ctx.window->UserPort->mp_SigBit;
            }
            wait_mask |= (ULONG)tg_platform_gui_drop_sigmask();
            /* 0.0.8 1b: while a transfer is active the loop must not sleep --
               each turn drains events, then pumps ONE chunk/part below. The
               network RPC inside the step paces the loop, so this is not a
               busy spin. */
            if (!tg_gui_session_transfer_busy()) {
                wake_signals = Wait(wait_mask);
            }
            /* INTUITICKS stop when a window is inactive. The existing VBLANK
               heartbeat must schedule photo work too, not only message polls. */
            if (timer_ok &&
                (wake_signals & (1UL << timer_port->mp_SigBit)) != 0UL) {
                photo_tick = 1;
            }
        }
        tg_gui_photo_viewer_drain(&viewer, &photo_tick,
                                  &interactive_event,
                                  &viewer_save_requested);
        if (viewer_save_requested && viewer.ctx.window != 0 &&
            (viewer.slot.id_hi != 0UL || viewer.slot.id_lo != 0UL)) {
            tg_gui_photo_save_begin(state, viewer.ctx.window, &backend,
                                    &photo_save, viewer.slot.id_hi,
                                    viewer.slot.id_lo);
        }
        while ((msg = (struct IntuiMessage *)GetMsg(ctx.window->UserPort)) !=
               0) {
            ULONG msg_class;
            UWORD msg_code;
            UWORD msg_qual;
            WORD mouse_x;
            WORD mouse_y;
            ULONG msg_secs;   /* IntuiMessage timestamp, for double-click detect */
            ULONG msg_micros;
            APTR key_menu_action = 0; /* Amiga+key mapped to a menu action */
#if defined(__amigaos4__)
            WORD wheel_y = 0;
#endif

            msg_class = msg->Class;
            msg_code = msg->Code;
            msg_qual = msg->Qualifier;
            mouse_x = msg->MouseX;
            mouse_y = msg->MouseY;
            msg_secs = msg->Seconds;
            msg_micros = msg->Micros;
            if (msg_class == IDCMP_INTUITICKS) {
                photo_tick = 1;
            } else {
                /* Any real queued event wins over background image work. */
                interactive_event = 1;
            }
            /* Feed REAL user input (keys, clicks, pointer motion) into the
               platform entropy ring -- the DRBG absorbs it on every generate.
               This is what makes the first-run auth-key DH benefit from the
               human at the keyboard even on the GUI-only path (the console
               already fed its stdin bytes). INTUITICKS et al. are skipped so
               the ring stays input-dominated. O(1), no hashing here. */
            if (msg_class == IDCMP_RAWKEY || msg_class == IDCMP_VANILLAKEY ||
                msg_class == IDCMP_MOUSEBUTTONS ||
                msg_class == IDCMP_MOUSEMOVE
#if defined(__amigaos4__)
                || msg_class == IDCMP_EXTENDEDMOUSE
#endif
                ) {
                tg_platform_note_input_event(
                    ((unsigned long)msg_class << 8) ^ (unsigned long)msg_code ^
                        ((unsigned long)msg_qual << 20),
                    ((unsigned long)(unsigned short)mouse_x << 16) |
                        (unsigned long)(unsigned short)mouse_y);
            }
#if defined(__amigaos4__)
            /* Read the wheel delta BEFORE ReplyMsg: the IntuiWheelData behind
               IAddress is only guaranteed valid until the message is replied. */
            if (msg_class == IDCMP_EXTENDEDMOUSE &&
                msg_code == IMSGCODE_INTUIWHEELDATA && msg->IAddress != 0) {
                wheel_y = ((struct IntuiWheelData *)msg->IAddress)->WheelY;
            }
#endif
            ReplyMsg((struct Message *)msg);

            /* Remember the last keystroke so the live poll can defer the
               (blocking) tick while you are actively typing -- see the
               IDCMP_INTUITICKS handler below. */
            if (msg_class == IDCMP_VANILLAKEY || msg_class == IDCMP_RAWKEY) {
                last_key_time = time(0);
            }
            /* Amiga+<key> is a menu shortcut, NEVER text. Intuition only turns
               it into a MENUPICK for the RIGHT Amiga and only while the menu
               strip is attached; otherwise it arrives as a plain VANILLAKEY and
               the composer was inserting it as a letter (OS3 field report:
               Amiga+C typed "c", Amiga+V typed "v"). Map it to the same action
               ids the menu uses, accept either Amiga key, and swallow the rest
               so no qualified key can ever be typed. */
            if (msg_class == IDCMP_VANILLAKEY &&
                (msg_qual & (IEQUALIFIER_LCOMMAND |
                             IEQUALIFIER_RCOMMAND)) != 0) {
                UWORD k = msg_code;

                if (k >= 'a' && k <= 'z') {
                    k = (UWORD)(k - 32); /* fold to the menu's upper-case key */
                }
                switch (k) {
                case 'C': key_menu_action = (APTR)TG_MENU_COPY; break;
                case 'V': key_menu_action = (APTR)TG_MENU_PASTE; break;
                case 'X': key_menu_action = (APTR)TG_MENU_CUT; break;
                case 'R': key_menu_action = (APTR)TG_MENU_REMOVE; break;
                case 'F': key_menu_action = (APTR)TG_MENU_SENDFILE; break;
                case 'P': key_menu_action = (APTR)TG_MENU_SENDPHOTO; break;
                case 'E': key_menu_action = (APTR)TG_MENU_EMOJI; break;
                case 'I': key_menu_action = (APTR)TG_MENU_ICONIFY; break;
                case 'Q': key_menu_action = (APTR)TG_MENU_QUIT; break;
                default: break; /* unknown shortcut: swallowed, never typed */
                }
                msg_class = 0; /* consumed: keep it out of every text field */
            }
#if defined(__amigaos4__)
            /* QEMU's emulated OS4 mouse delivers the wheel only as
               IDCMP_EXTENDEDMOUSE, not the NewMouse RAWKEY 0x7A/0x7B that real
               hardware + the other platforms send (iBrowse handles
               IDCMP_EXTENDEDMOUSE, which is why it scrolls under QEMU and we did
               not). Translate it into those RAWKEY codes so the wheel handler
               below reuses all its panel/scroll logic unchanged. */
            if (msg_class == IDCMP_EXTENDEDMOUSE && wheel_y != 0) {
                msg_class = IDCMP_RAWKEY;
                msg_code = (wheel_y < 0) ? (UWORD)0x7A : (UWORD)0x7B;
            }
#endif

            /* A keystroke (or wheel, already mapped to RAWKEY above) while the
               context menu is open simply closes it and is consumed -- standard
               menu behaviour, and it avoids ESC also quitting the app. */
            if (state->ctx_visible &&
                (msg_class == IDCMP_VANILLAKEY || msg_class == IDCMP_RAWKEY)) {
                state->ctx_visible = 0;
                tg_gui_window_paint(state, &backend);
                continue;
            }

            if (msg_class == IDCMP_CLOSEWINDOW) {
#if defined(__amigaos4__)
                if (msg_code == 1) { /* the iconify gadget, not a real close */
                    tg_gui_log("window: iconify gadget");
                    done = 2; /* park on the AppIcon, same as the menu item */
                } else
#endif
                if (tg_gui_session_transfer_busy()) {
                    /* First close = cancel the running transfer, not quit;
                       the pump unwinds it and reports "cancelled". A second
                       close then quits as usual. */
                    tg_gui_log("window: close gadget = cancel transfer");
                    tg_gui_session_transfer_cancel();
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       "Cancelling...");
                    tg_gui_window_paint(state, &backend);
                } else if (photo_save.pending) {
                    tg_gui_log("window: close gadget = cancel photo save");
                    tg_gui_photo_save_cancel(state, &backend, &photo_save);
                } else {
                    tg_gui_log("window: close gadget");
                    done = 1;
                }
            } else if (msg_class == IDCMP_VANILLAKEY && msg_code == 27 &&
                       tg_gui_session_transfer_busy()) {
                /* ESC during a transfer = cancel it (everywhere: composer,
                   search and chat ESC meanings resume once it is idle). */
                tg_gui_session_transfer_cancel();
                tg_gui_window_copy(state->status, sizeof(state->status),
                                   "Cancelling...");
                tg_gui_window_paint(state, &backend);
            } else if (msg_class == IDCMP_VANILLAKEY && msg_code == 27 &&
                       photo_save.pending) {
                tg_gui_photo_save_cancel(state, &backend, &photo_save);
            } else if (msg_class == IDCMP_VANILLAKEY &&
                       state->mode != TG_GUI_MODE_CHAT) {
                /* A login screen owns the keyboard until the session opens. */
                tg_gui_window_login_key(state, msg_code, &backend, &done,
                                        &caret_ticks);
            } else if (msg_class == IDCMP_VANILLAKEY && state->search_active) {
                /* The sidebar search box owns the keyboard while focused: type a
                   name, ENTER runs an online search and opens the top match,
                   ESC cancels, BACKSPACE deletes. */
                if (msg_code == 27) { /* ESC */
                    if (state->forward_pick_active) {
                        tg_gui_window_forward_restore(state);
                        tg_gui_window_copy(state->status,
                                           sizeof(state->status),
                                           "Forward cancelled");
                    } else {
                        state->search_active = 0;
                        state->search_query[0] = '\0';
                        state->search_caret = 0;
                        if (state->in_search || state->in_filter) {
                            state->in_search = 0; /* picker or local filter: */
                            state->in_filter = 0; /* restore the real chats  */
                            tg_gui_session_refresh_chats();
                        }
                        tg_gui_window_copy(state->status,
                                           sizeof(state->status),
                                           "Live - F1-F10 chats, Q quits");
                    }
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == 8) { /* BACKSPACE: delete BEFORE the caret */
                    unsigned long n;
                    int sc;

                    n = (unsigned long)strlen(state->search_query);
                    sc = state->search_caret;
                    if (sc < 0 || sc > (int)n) {
                        sc = (int)n;
                    }
                    if (sc > 0) { /* delete the char BEFORE the caret */
                        memmove(state->search_query + sc - 1,
                                state->search_query + sc, n - (unsigned long)sc
                                + 1UL);
                        state->search_caret = sc - 1;
                        last_key_time = time(0);
                        tg_gui_window_filter_chats(state, &backend);
                    }
                } else if (msg_code == 127) { /* Canc/Del: delete AT the caret */
                    unsigned long n;
                    int sc;

                    n = (unsigned long)strlen(state->search_query);
                    sc = state->search_caret;
                    if (sc < 0 || sc > (int)n) {
                        sc = (int)n;
                    }
                    if (sc < (int)n) { /* forward-delete: pull the tail one left */
                        memmove(state->search_query + sc,
                                state->search_query + sc + 1,
                                n - (unsigned long)sc);
                        last_key_time = time(0);
                        tg_gui_window_filter_chats(state, &backend);
                    }
                } else if (msg_code == 13 || msg_code == 10) { /* ENTER: search */
                    if (state->in_search && state->chat_count > 0 &&
                        state->selected_chat >= 0 &&
                        state->selected_chat < state->chat_count) {
                        /* The online picker already contains concrete results:
                           ENTER chooses the highlighted row instead of rerunning
                           the same network search. */
                        if (state->forward_pick_active) {
                            tg_gui_window_forward_online_result(
                                state, &backend, state->selected_chat);
                        } else {
                            int result_index = state->selected_chat;

                            state->in_search = 0;
                            state->search_active = 0;
                            state->search_query[0] = '\0';
                            (void)tg_gui_session_search_open_result(result_index,
                                                                    stdout);
                            tg_gui_window_copy(
                                state->status, sizeof(state->status),
                                "Live - F1-F10 chats, Q quits");
                            tg_gui_window_paint(state, &backend);
                        }
                    } else if (state->in_filter && state->chat_count > 0 &&
                        state->selected_chat >= 0 &&
                        state->selected_chat < state->chat_count &&
                        state->chats[state->selected_chat].index != 0UL) {
                        /* Open or choose the highlighted LOCAL match. */
                        if (state->forward_pick_active) {
                            tg_gui_window_forward_to_index(
                                state, &backend,
                                state->chats[state->selected_chat].index,
                                state->chats[state->selected_chat].name);
                        } else {
                            tg_gui_window_open_by_index(
                                state, &backend,
                                state->chats[state->selected_chat].index);
                            tg_gui_window_copy(
                                state->status, sizeof(state->status),
                                "Live - F1-F10 chats, Q quits");
                            tg_gui_window_paint(state, &backend);
                        }
                    } else {
                        /* The "Search Telegram..." row (or no local match):
                           the online search, exactly as before. */
                        tg_gui_window_run_search(state, &backend, 1);
                    }
                } else if (msg_code >= 32 && msg_code < 256) { /* printable */
                    unsigned long n;
                    int sc;

                    n = (unsigned long)strlen(state->search_query);
                    sc = state->search_caret;
                    if (sc < 0 || sc > (int)n) {
                        sc = (int)n;
                    }
                    if (n + 1UL < sizeof(state->search_query)) {
                        memmove(state->search_query + sc + 1,
                                state->search_query + sc,
                                n - (unsigned long)sc + 1UL);
                        state->search_query[sc] = (char)msg_code;
                        state->search_caret = sc + 1;
                        last_key_time = time(0);
                        tg_gui_window_filter_chats(state, &backend);
                    }
                }
            } else if (msg_class == IDCMP_VANILLAKEY && state->composing) {
                int old_input_h;
                int old_mention_active;

                old_input_h = tg_gui_input_layout_height(state, &backend);
                old_mention_active = state->mention_active;
                /* Composing: keys edit the input line; RETURN sends, ESC
                   cancels, BACKSPACE deletes. While the '@' mention popup is
                   up, RETURN/TAB insert the highlighted username instead (the
                   NEXT return sends) and ESC only closes the popup. */
                if (state->emoji_active &&
                    (msg_code == 13 || msg_code == 10 || msg_code == 27)) {
                    /* The emoji picker owns ENTER and ESC while it is up:
                       ENTER inserts and keeps the panel open for the next
                       one, ESC closes it. The arrows are raw keys and are
                       taken in the raw key chain below. */
                    if (msg_code == 27) {
                        tg_gui_emoji_close(state);
                    } else {
                        (void)tg_gui_emoji_pick(state);
                    }
                    tg_gui_window_paint(state, &backend);
                } else if ((msg_code == 13 || msg_code == 10 || msg_code == 9) &&
                    state->mention_active) {
                    tg_gui_window_mention_complete(state);
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == 27 && state->mention_active) {
                    state->mention_active = 0;
                    state->mention_count = 0;
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == 13 || msg_code == 10) {
                    if (state->input[0] != '\0') {
                        if (state->edit_to_id != 0UL) {
                            state->in_sel_active = 0;
                            /* Edit mode: save the edit, then leave edit mode (the
                               bubble is updated in place by the session call). */
                            (void)tg_gui_session_edit(state->input,
                                                      state->edit_to_id, stdout);
                            state->edit_to_id = 0UL;
                        } else if (tg_gui_session_send(state->input,
                                                       state->reply_to_id,
                                                       stdout) == 0) {
                            tg_gui_history_add(state, state->input);
                            state->in_sel_active = 0;
                            state->reply_to_id = 0UL; /* clear only on success */
                            state->reply_sender[0] = '\0';
                            state->reply_snippet[0] = '\0';
                            tg_gui_window_jump_to_bottom(state, &backend,
                                                         &older_exhausted,
                                                         &older_cooldown);
                        }
                        state->input[0] = '\0';
                    }
                    state->input_caret = 0;
                    state->history_pos = -1;
                    state->history_draft[0] = '\0';
                    /* Keep focus in the composer so the next message can be
                       typed without re-clicking; re-prime the caret blink. */
                    state->composing = 1;
                    state->cursor_on = 1;
                    caret_ticks = 0;
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       "Type - ENTER sends, ESC cancels");
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == 27) {
                    state->input[0] = '\0';
                    state->input_caret = 0;
                    state->history_pos = -1;
                    state->composing = 0;
                    /* Leaving the composer drops any pending reply/edit too. */
                    state->reply_to_id = 0UL;
                    state->reply_sender[0] = '\0';
                    state->reply_snippet[0] = '\0';
                    state->edit_to_id = 0UL;
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       "Live - F1-F10 chats, Q quits");
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == 8) { /* BACKSPACE: delete BEFORE the caret */
                    unsigned long n;
                    unsigned long c;

                    if (tg_gui_window_input_delete_sel(state)) {
                        /* a selection consumes the keypress whole */
                        tg_gui_window_mention_refresh(state);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    } else {
                    n = (unsigned long)strlen(state->input);
                    c = (unsigned long)state->input_caret;
                    if (c > n) {
                        c = n;
                    }
                    if (c > 0UL) {
                        /* delete the unit before the caret, keeping the NUL:
                           one byte, or the two of an emoji pair */
                        unsigned long del = 1UL;

                        if (c >= 2UL &&
                            tg_gui_emoji_pair_at(state->input, n, c - 2UL, 0)) {
                            del = 2UL;
                        }
                        memmove(&state->input[c - del], &state->input[c],
                                n - c + 1UL);
                        state->input_caret = (int)(c - del);
                        tg_gui_window_mention_refresh(state);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    }
                    }
                } else if (msg_code == 127) { /* Canc/Del: delete AT the caret */
                    unsigned long n;
                    unsigned long c;

                    if (tg_gui_window_input_delete_sel(state)) {
                        tg_gui_window_mention_refresh(state);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    } else {
                    n = (unsigned long)strlen(state->input);
                    c = (unsigned long)state->input_caret;
                    if (c < n) {
                        /* forward-delete: pull the tail (incl. NUL) one left,
                           the caret stays put. Backspace (0x08) deletes left;
                           this splits the two so Canc is not folded into it. */
                        {
                            unsigned long unit =
                                tg_gui_text_unit_len(state->input, n, c);

                            memmove(&state->input[c], &state->input[c + unit],
                                    n - (c + unit) + 1UL); /* tail + NUL */
                        }
                        tg_gui_window_mention_refresh(state);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    }
                    }
                } else if (msg_code >= 32 && msg_code < 256) {
                    unsigned long n;
                    unsigned long c;

                    /* typing REPLACES an active selection (classic field) */
                    (void)tg_gui_window_input_delete_sel(state);
                    n = (unsigned long)strlen(state->input);
                    c = (unsigned long)state->input_caret;
                    if (c > n) {
                        c = n;
                    }
                    if (n + 1UL < (unsigned long)sizeof(state->input)) {
                        /* insert at the caret, shifting the tail (incl. NUL) */
                        memmove(&state->input[c + 1UL], &state->input[c],
                                n - c + 1UL);
                        state->input[c] = (char)msg_code;
                        state->input_caret = (int)(c + 1UL);
                        tg_gui_window_mention_refresh(state);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    }
                }
            } else if (msg_class == IDCMP_VANILLAKEY) {
                if (msg_code == 'q' || msg_code == 'Q' || msg_code == 27) {
                    done = 1;
                } else if ((msg_code == 13 || msg_code == 10) &&
                           state->nav_chat >= 0 &&
                           state->nav_chat != state->selected_chat &&
                           state->nav_chat < state->chat_count &&
                           tg_gui_session_is_open()) {
                    /* RETURN opens the arrow-focused chat. */
                    tg_gui_window_open_selection(state, state->nav_chat,
                                                 &backend);
                } else if ((msg_code == 13 || msg_code == 10) &&
                           tg_gui_session_is_open()) {
                    /* RETURN starts composing a message for the open chat. */
                    state->composing = 1;
                    state->input_caret = (int)strlen(state->input);
                    state->cursor_on = 1;
                    caret_ticks = 0;
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       "Type - ENTER sends, ESC cancels");
                    tg_gui_window_paint(state, &backend);
                }
                /* Chat selection is on the function keys now (IDCMP_RAWKEY). */
            } else if (msg_class == IDCMP_RAWKEY &&
                       (msg_code == 0x7A || msg_code == 0x7B) &&
                       state->mode == TG_GUI_MODE_CHAT) {
                /* Mouse wheel (NewMouse RAWKEY 0x7A up / 0x7B down): scroll the
                   panel under the pointer. The up-transition arrives as
                   0xFA/0xFB and is ignored by the strict == tests (fires once).
                   Works while composing too (no gate). */
                int sw;
                int hx;

                sw = tg_gui_sidebar_w(ctx.inner_w);
                hx = (int)mouse_x - ctx.origin_x;
                if (hx < sw) {
                    state->chat_scroll += (msg_code == 0x7A) ? -3 : 3;
                    if (state->chat_scroll < 0) {
                        state->chat_scroll = 0;
                    }
                } else {
                    /* 0x7A reveals older history (scroll up), 0x7B newer. */
                    state->transcript_scroll += (msg_code == 0x7A) ? (3 * ctx.line_h) : (-3 * ctx.line_h);
                    if (state->transcript_scroll < 0) {
                        state->transcript_scroll = 0;
                    }
                    if (msg_code == 0x7A) {
                        want_older = 1; /* may have reached the top: paged below */
                    }
                }
                scroll_dirty = 1;
            } else if (msg_class == IDCMP_RAWKEY && state->search_active &&
                       (msg_code == 0x4C || msg_code == 0x4D) &&
                       (state->in_filter || state->in_search) &&
                       state->chat_count > 0) {
                /* Arrows walk the result list (local filter or online
                   picker); ENTER opens the highlighted row. */
                int sel = state->selected_chat;

                sel += (msg_code == 0x4D) ? 1 : -1;
                if (sel < 0) {
                    sel = 0;
                }
                if (sel >= state->chat_count) {
                    sel = state->chat_count - 1;
                }
                state->selected_chat = sel;
                state->chat_scroll_to_sel = 1;
                tg_gui_window_paint(state, &backend);
            } else if (msg_class == IDCMP_RAWKEY && state->search_active &&
                       (msg_code == 0x4F || msg_code == 0x4E)) {
                /* F8: arrows move the search caret (insert point). */
                int n = (int)strlen(state->search_query);
                int sc = state->search_caret;

                if (sc < 0 || sc > n) {
                    sc = n;
                }
                if (msg_code == 0x4F && sc > 0) {
                    --sc;
                } else if (msg_code == 0x4E && sc < n) {
                    ++sc;
                }
                state->search_caret = sc;
                state->cursor_on = 1;
                caret_ticks = 0;
                tg_gui_window_paint_caret(state, &backend);
            } else if (msg_class == IDCMP_RAWKEY && state->composing &&
                       state->mode == TG_GUI_MODE_CHAT) {
                int old_input_h;
                int old_mention_active;

                old_input_h = tg_gui_input_layout_height(state, &backend);
                old_mention_active = state->mention_active;
                /* While composing, LEFT/RIGHT move the caret within the input.
                   The key-up event arrives as code|0x80, so the strict == tests
                   fire exactly once per press. With the '@' mention popup up,
                   UP/DOWN move its highlight instead of recalling history. */
                if ((msg_code == 0x4C || msg_code == 0x4D) &&
                    state->mention_active) {
                    if (msg_code == 0x4C) { /* up */
                        state->mention_sel = (state->mention_sel > 0)
                                                 ? state->mention_sel - 1
                                                 : state->mention_count - 1;
                    } else {                 /* down */
                        state->mention_sel =
                            (state->mention_sel + 1 < state->mention_count)
                                ? state->mention_sel + 1
                                : 0;
                    }
                    tg_gui_window_paint_caret(state, &backend);
                } else if (state->emoji_active &&
                           (msg_code == 0x4C || msg_code == 0x4D ||
                            msg_code == 0x4E || msg_code == 0x4F)) {
                    /* The emoji panel walks its grid with the arrows. */
                    tg_gui_emoji_move(state,
                                      msg_code == 0x4F ? -1 : msg_code == 0x4E ? 1 : 0,
                                      msg_code == 0x4C ? -1 : msg_code == 0x4D ? 1 : 0);
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == 0x4F || msg_code == 0x4E) {
                    /* cursor left/right; with SHIFT they grow/shrink the
                       composer selection anchored where Shift was first
                       pressed (the classic text-field gesture). */
                    int shifted =
                        (msg_qual &
                         (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
                    int changed = 0;

                    if (shifted && !state->in_sel_active) {
                        state->in_sel_active = 1;
                        state->in_sel_anchor = state->input_caret;
                    } else if (!shifted && state->in_sel_active) {
                        state->in_sel_active = 0;
                        changed = 1;
                    }
                    if (msg_code == 0x4F && state->input_caret > 0) {
                        state->input_caret--;
                        if (state->input_caret > 0 &&
                            tg_gui_emoji_pair_at(state->input,
                                                 strlen(state->input),
                                                 (unsigned long)state->input_caret - 1UL, 0)) {
                            state->input_caret--; /* over the whole pair */
                        }
                        changed = 1;
                    } else if (msg_code == 0x4E &&
                               state->input_caret <
                                   (int)strlen(state->input)) {
                        state->input_caret +=
                            (int)tg_gui_text_unit_len(state->input,
                                                      strlen(state->input),
                                                      (unsigned long)state->input_caret);
                        changed = 1;
                    }
                    if (shifted &&
                        state->input_caret == state->in_sel_anchor) {
                        state->in_sel_active = 0; /* collapsed back */
                    }
                    if (changed) {
                        tg_gui_window_mention_refresh(state);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    }
                } else if (msg_code == 0x4C) { /* cursor up: older sent line */
                    if (state->history_count > 0) {
                        int hidx;

                        if (state->history_pos < 0) {
                            /* entering recall: stash the live draft */
                            tg_gui_window_copy(state->history_draft,
                                               sizeof(state->history_draft),
                                               state->input);
                            hidx = state->history_count - 1;
                        } else if (state->history_pos > 0) {
                            hidx = state->history_pos - 1;
                        } else {
                            hidx = 0;
                        }
                        state->history_pos = hidx;
                        tg_gui_window_copy(state->input, sizeof(state->input),
                                           state->history[hidx]);
                        state->input_caret = (int)strlen(state->input);
                        state->in_sel_active = 0;
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    }
                } else if (msg_code == 0x4D) { /* cursor down: newer sent line */
                    state->in_sel_active = 0; /* input is about to be rebuilt */
                    if (state->history_pos >= 0) {
                        int hidx;

                        hidx = state->history_pos + 1;
                        if (hidx >= state->history_count) {
                            /* past the newest: restore the live draft */
                            tg_gui_window_copy(state->input,
                                               sizeof(state->input),
                                               state->history_draft);
                            state->history_pos = -1;
                        } else {
                            state->history_pos = hidx;
                            tg_gui_window_copy(state->input,
                                               sizeof(state->input),
                                               state->history[hidx]);
                        }
                        state->input_caret = (int)strlen(state->input);
                        tg_gui_window_paint_composer_edit(
                            state, &backend, old_input_h, old_mention_active);
                    }
                }
            } else if (msg_class == IDCMP_RAWKEY && !state->composing &&
                       !state->search_active && state->mode == TG_GUI_MODE_CHAT) {
                /* F1..F10 (rawkey 0x50..0x59) pick chats 1..10; Shift adds 10
                   for 11..20 -- matching the console's F-key selection. (Disabled
                   while either local or online search is up: those rows are a
                   projection, so an F-key could change the forwarding source or
                   open a bogus chat and strand the picker.) */
                if (msg_code >= 0x50 && msg_code <= 0x59 &&
                    state->chat_count > 0) {
                    int idx;

                    idx = (int)(msg_code - 0x50);
                    if ((msg_qual &
                         (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0) {
                        idx += 10;
                    }
                    if (idx < state->chat_count &&
                        idx != state->selected_chat) {
                        tg_gui_window_open_selection(state, idx, &backend);
                    }
                } else if (msg_code == 0x4C || msg_code == 0x4D) {
                    /* Cursor up/down act on the PANEL UNDER THE POINTER --
                       the same rule the NewMouse wheel already follows.
                       This matters beyond taste: many setups deliver the
                       wheel AS these cursor rawkeys, so binding them to the
                       chat list unconditionally made wheel-scrolling the
                       transcript drag the sidebar focus along (regression
                       report). Sidebar side: walk the chat list, ENTER
                       opens. Transcript side: scroll the messages. */
                    int over_sidebar =
                        ((int)mouse_x - ctx.origin_x) <
                        tg_gui_sidebar_w(ctx.inner_w);

                    if (over_sidebar && state->chat_count > 0) {
                        int nv = (state->nav_chat >= 0)
                                     ? state->nav_chat
                                     : state->selected_chat;

                        nv += (msg_code == 0x4D) ? 1 : -1;
                        if (nv < 0) {
                            nv = 0;
                        }
                        if (nv >= state->chat_count) {
                            nv = state->chat_count - 1;
                        }
                        state->nav_chat = nv;
                        state->chat_scroll_to_sel = 1;
                        tg_gui_window_paint(state, &backend);
                    } else {
                        if (msg_code == 0x4C) { /* up: older messages */
                            state->transcript_scroll += 3 * ctx.line_h;
                            want_older = 1;
                        } else {                /* down: newer messages */
                            state->transcript_scroll -= 3 * ctx.line_h;
                            if (state->transcript_scroll < 0) {
                                state->transcript_scroll = 0;
                            }
                        }
                        scroll_dirty = 1;
                    }
                } else if (msg_code == 0x46) { /* Del: remove selected chat (confirm) */
                    tg_gui_window_remove_selected(state, ctx.window, &backend);
                }
            } else if (msg_class == IDCMP_MENUPICK || key_menu_action != 0) {
                UWORD mnum;

                tg_gui_log("menu: pick");
                mnum = msg_code;
                for (;;) {
                    struct MenuItem *item = 0;

                    {
                        APTR ud;

                        if (key_menu_action != 0) {
                            /* keyboard path: one action, no NextSelect chain */
                            ud = key_menu_action;
                            key_menu_action = 0;
                        } else {
                            if (mnum == MENUNULL) {
                                break;
                            }
                            item = ItemAddress(menu, mnum);
                            if (item == 0) {
                                break;
                            }
                            ud = GTMENUITEM_USERDATA(item);
                        }
                        if (ud == (APTR)TG_MENU_ABOUT) {
                            tg_gui_amiga_easyreq(ctx.window,
                                                 "About Telegram Amiga",
                                                 tg_gui_about_text);
                        } else if (ud == (APTR)TG_MENU_HELP) {
                            tg_gui_amiga_easyreq(ctx.window, "Telegram Amiga Help",
                                                 tg_gui_help_text);
                        } else if (ud == (APTR)TG_MENU_REMOVE) {
                            tg_gui_window_remove_selected(state, ctx.window,
                                                          &backend);
                        } else if (ud == (APTR)TG_MENU_SENDFILE) {
                            tg_gui_window_send_file(state, ctx.window,
                                                    &backend);
                        } else if (ud == (APTR)TG_MENU_EMOJI) {
                            if (state->emoji_active) {
                                tg_gui_emoji_close(state);
                            } else {
                                tg_gui_emoji_open(state);
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_SENDPHOTO) {
                            tg_gui_window_send_photo(state, ctx.window,
                                                     &backend);
                        } else if (ud == (APTR)TG_MENU_COPY) {
                            /* Copy the highlighted message's text (issue #5).
                               Selection = the row the user last right-clicked
                               or picked; without one, say what to do. */
                            static char selbuf[TG_GUI_MSG_TEXT_MAX];
                            int sel = state->selected_msg;
                            const char *src = 0;

                            if (state->composing && state->in_sel_active) {
                                /* composer selection: copy [anchor..caret] */
                                long a = (long)state->in_sel_anchor;
                                long b = (long)state->input_caret;
                                long lo = a < b ? a : b;
                                long hi = a > b ? a : b;
                                long tl = (long)strlen(state->input);

                                if (lo < 0) {
                                    lo = 0;
                                }
                                if (hi > tl) {
                                    hi = tl;
                                }
                                if (hi > lo &&
                                    (unsigned long)(hi - lo) <
                                        sizeof(selbuf)) {
                                    memcpy(selbuf, state->input + lo,
                                           (unsigned long)(hi - lo));
                                    selbuf[hi - lo] = '\0';
                                    src = selbuf;
                                }
                            }
                            if (src == 0 &&
                                tg_gui_selection_get(state, selbuf,
                                                     sizeof(selbuf))) {
                                src = selbuf; /* the dragged range wins */
                            } else if (src == 0 && sel >= 0 &&
                                       sel < state->message_count &&
                                       state->messages[sel].text[0] !=
                                           '\0') {
                                src = state->messages[sel].text;
                            }
                            if (src != 0 && tg_gui_clip_write_text(src)) {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Copied to clipboard");
                            } else {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   sel < 0
                                                       ? "Right-click a message first"
                                                       : "Nothing to copy");
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_CUT) {
                            /* Cut the focused input line to the clipboard:
                               the search box when active, else the composer.
                               Completes the cut/copy/paste trio (issue #5). */
                            char *field = state->search_active
                                              ? state->search_query
                                              : state->input;

                            if (!state->search_active && state->composing &&
                                state->in_sel_active) {
                                /* cut ONLY the selected composer range */
                                static char cutbuf[TG_GUI_MSG_TEXT_MAX];
                                long a = (long)state->in_sel_anchor;
                                long b = (long)state->input_caret;
                                long lo = a < b ? a : b;
                                long hi = a > b ? a : b;
                                long tl = (long)strlen(state->input);

                                if (lo < 0) {
                                    lo = 0;
                                }
                                if (hi > tl) {
                                    hi = tl;
                                }
                                if (hi > lo &&
                                    (unsigned long)(hi - lo) <
                                        sizeof(cutbuf)) {
                                    memcpy(cutbuf, state->input + lo,
                                           (unsigned long)(hi - lo));
                                    cutbuf[hi - lo] = '\0';
                                    if (tg_gui_clip_write_text(cutbuf)) {
                                        (void)
                                        tg_gui_window_input_delete_sel(state);
                                        tg_gui_window_mention_refresh(state);
                                        tg_gui_window_copy(
                                            state->status,
                                            sizeof(state->status),
                                            "Cut to clipboard");
                                    } else {
                                        tg_gui_window_copy(
                                            state->status,
                                            sizeof(state->status),
                                            "Copy failed");
                                    }
                                } else {
                                    tg_gui_window_copy(state->status,
                                                       sizeof(state->status),
                                                       "Nothing to cut");
                                }
                                tg_gui_window_paint(state, &backend);
                            } else if (field[0] != '\0' &&
                                tg_gui_clip_write_text(field)) {
                                field[0] = '\0';
                                if (state->search_active) {
                                    state->search_caret = 0;
                                    /* Refilter like every typing path does:
                                       the sidebar kept the old matches until
                                       the next keystroke. */
                                    tg_gui_window_filter_chats(state, &backend);
                                } else {
                                    state->input_caret = 0;
                                    tg_gui_window_mention_refresh(state);
                                }
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Cut to clipboard");
                            } else {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Nothing to cut");
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_PASTE) {
                            /* Paste the clipboard's FTXT at the caret: into
                               the search box when it is active, else into the
                               composer. LINE BREAKS ARE KEPT (a pasted text
                               file used to arrive as one paragraph, which
                               only looked right here because our renderer
                               re-wraps it -- other clients showed the
                               original line structure gone): CR / CRLF are
                               normalised to LF, which the composer wraps on
                               and Telegram carries as a real newline. Tabs
                               become spaces, other control bytes are
                               dropped, and the single-line search box
                               flattens what is left below. */
                            static char clip[TG_GUI_MSG_TEXT_MAX];
                            unsigned long got =
                                tg_gui_clip_read_text(clip, sizeof(clip));
                            unsigned long src, dst = 0UL;

                            for (src = 0UL; src < got; ++src) {
                                unsigned char cch = (unsigned char)clip[src];

                                if (cch == '\r') {
                                    /* CRLF counts once. */
                                    if (src + 1UL < got &&
                                        clip[src + 1UL] == '\n') {
                                        continue;
                                    }
                                    clip[dst++] = '\n';
                                } else if (cch == '\n') {
                                    clip[dst++] = '\n';
                                } else if (cch == '\t') {
                                    clip[dst++] = ' ';
                                } else if (cch >= 32) {
                                    clip[dst++] = (char)cch;
                                }
                            }
                            if (state->search_active) {
                                /* One-line field: no breaks in there. */
                                unsigned long f;

                                for (f = 0UL; f < dst; ++f) {
                                    if (clip[f] == '\n') {
                                        clip[f] = ' ';
                                    }
                                }
                            }
                            got = dst;
                            clip[got] = '\0';
                            if (got == 0UL) {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Clipboard is empty");
                                tg_gui_window_paint(state, &backend);
                            } else if (state->search_active) {
                                unsigned long n = (unsigned long)strlen(
                                    state->search_query);
                                unsigned long c =
                                    (unsigned long)state->search_caret;
                                unsigned long room =
                                    sizeof(state->search_query) - 1UL - n;
                                unsigned long p = got > room ? room : got;

                                if (c > n) {
                                    c = n;
                                }
                                memmove(&state->search_query[c + p],
                                        &state->search_query[c], n - c + 1UL);
                                memcpy(&state->search_query[c], clip, p);
                                state->search_caret = (int)(c + p);
                                /* Filter now (and let it write the status):
                                   nothing runs a search on a pause any more,
                                   so the pasted text used to sit over a
                                   sidebar that had not moved. */
                                tg_gui_window_filter_chats(state, &backend);
                            } else if (tg_gui_session_is_open() &&
                                       state->mode == TG_GUI_MODE_CHAT) {
                                unsigned long n;
                                unsigned long c;
                                unsigned long room;
                                unsigned long p;

                                (void)tg_gui_window_input_delete_sel(state);
                                n = (unsigned long)strlen(state->input);
                                c = (unsigned long)state->input_caret;
                                room = sizeof(state->input) - 1UL - n;
                                p = got > room ? room : got;

                                if (c > n) {
                                    c = n;
                                }
                                memmove(&state->input[c + p],
                                        &state->input[c], n - c + 1UL);
                                memcpy(&state->input[c], clip, p);
                                state->input_caret = (int)(c + p);
                                state->composing = 1;
                                state->cursor_on = 1;
                                caret_ticks = 0;
                                tg_gui_window_mention_refresh(state);
                                if (p < got) {
                                    /* The composer took what it could hold.
                                       Saying so beats dropping the rest in
                                       silence, which read as a broken paste
                                       (issue #14). */
                                    char cut[64];

                                    sprintf(cut,
                                            "Pasted %lu of %lu characters "
                                            "(composer full)",
                                            (unsigned long)p,
                                            (unsigned long)got);
                                    tg_gui_window_copy(state->status,
                                                       sizeof(state->status),
                                                       cut);
                                } else {
                                    tg_gui_window_copy(state->status,
                                                       sizeof(state->status),
                                                       "Pasted");
                                }
                                tg_gui_window_paint(state, &backend);
                            } else {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Open a chat first");
                                tg_gui_window_paint(state, &backend);
                            }
                        } else if (ud == (APTR)TG_MENU_ICONIFY) {
                            /* Tear down window (and own screen) via the normal
                               path; the outer loop parks on an AppIcon. */
                            done = 2;
                        } else if (ud == (APTR)TG_MENU_DLDIR) {
                            tg_gui_window_pick_download_dir(state, ctx.window,
                                                            &backend);
                        } else if (ud == (APTR)TG_MENU_INLINEPHOTOS) {
                            state->inline_photos = !state->inline_photos;
                            state->inline_photos_explicit = 1;
                            state->inline_photos_default_resolved = 1;
                            tg_gui_session_set_inline_photos(
                                state->inline_photos);
                            tg_gui_photo_slots_reset();
                            if (tg_gui_photo_preferences_save(
                                    "data/telegram-photos.txt",
                                    state->inline_photos,
                                    state->inline_photos_explicit,
                                    state->photo_dither,
                                    state->photo_cache_limit_mb) != 0) {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Could not save photo setting");
                            } else {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    state->inline_photos
                                        ? "Inline photos enabled"
                                        : "Inline photos disabled");
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_DITHER_FULL ||
                                   ud == (APTR)TG_MENU_DITHER_LIGHT ||
                                   ud == (APTR)TG_MENU_DITHER_OFF) {
                            unsigned long viewer_hi;
                            unsigned long viewer_lo;

                            state->photo_dither =
                                ud == (APTR)TG_MENU_DITHER_LIGHT
                                    ? TG_GUI_PHOTO_DITHER_LIGHT
                                    : ud == (APTR)TG_MENU_DITHER_OFF
                                          ? TG_GUI_PHOTO_DITHER_OFF
                                          : TG_GUI_PHOTO_DITHER_FULL;
                            ctx.photo_dither = state->photo_dither;
                            viewer.ctx.photo_dither = state->photo_dither;
                            viewer_hi = viewer.slot.id_hi;
                            viewer_lo = viewer.slot.id_lo;
                            tg_gui_photo_slots_reset();
                            if (viewer.ctx.window != 0 &&
                                (viewer_hi != 0UL || viewer_lo != 0UL)) {
                                tg_gui_photo_slot_clear(&viewer.slot);
                                viewer.slot.id_hi = viewer_hi;
                                viewer.slot.id_lo = viewer_lo;
                                tg_gui_photo_viewer_paint(&viewer);
                            }
                            tg_gui_menu_set_photo_dither(
                                menu, state->photo_dither);
                            if (tg_gui_photo_preferences_save(
                                    "data/telegram-photos.txt",
                                    state->inline_photos,
                                    state->inline_photos_explicit,
                                    state->photo_dither,
                                    state->photo_cache_limit_mb) != 0) {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    "Could not save photo setting");
                            } else {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    state->photo_dither ==
                                            TG_GUI_PHOTO_DITHER_LIGHT
                                        ? "Photo dithering: Light"
                                        : state->photo_dither ==
                                                  TG_GUI_PHOTO_DITHER_OFF
                                              ? "Photo dithering: Off"
                                              : "Photo dithering: Full");
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_CACHE_10 ||
                                   ud == (APTR)TG_MENU_CACHE_50 ||
                                   ud == (APTR)TG_MENU_CACHE_200 ||
                                   ud == (APTR)TG_MENU_CACHE_UNLIMITED) {
                            state->photo_cache_limit_mb =
                                ud == (APTR)TG_MENU_CACHE_10 ? 10UL :
                                ud == (APTR)TG_MENU_CACHE_200 ? 200UL :
                                ud == (APTR)TG_MENU_CACHE_UNLIMITED
                                    ? TG_GUI_PHOTO_CACHE_UNLIMITED_MB : 50UL;
                            tg_gui_photo_cache_set_limit(
                                state->photo_cache_limit_mb);
                            tg_gui_menu_set_photo_cache_limit(
                                menu, state->photo_cache_limit_mb);
                            if (tg_gui_photo_preferences_save(
                                    "data/telegram-photos.txt",
                                    state->inline_photos,
                                    state->inline_photos_explicit,
                                    state->photo_dither,
                                    state->photo_cache_limit_mb) != 0) {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    "Could not save photo setting");
                            } else if (state->photo_cache_limit_mb ==
                                       TG_GUI_PHOTO_CACHE_UNLIMITED_MB) {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    "Photo cache limit: Unlimited");
                            } else {
                                char line[48];

                                sprintf(line, "Photo cache limit: %lu MB",
                                        state->photo_cache_limit_mb);
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status), line);
                            }
                            if (tg_gui_photo_cache_pending()) {
                                photo_fast_wake = 1;
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_CACHE_CLEAR) {
                            if (tg_gui_amiga_confirm_clear_photo_cache(
                                    ctx.window) == 1) {
                                tg_gui_photo_cache_request_clear(
                                    state, &viewer);
                                photo_fast_wake = 1;
                                tg_gui_window_paint(state, &backend);
                            }
                        } else if (ud == (APTR)TG_MENU_RELOAD) {
                            /* Re-page the dialog list on demand (start-up
                               no longer refetches). Blocking but bounded:
                               a few 30-dialog pages. */
                            int rrc;

                            tg_gui_window_copy(state->status,
                                               sizeof(state->status),
                                               "Reloading chat list...");
                            tg_gui_window_paint(state, &backend);
                            rrc = tg_gui_session_reload_chat_list(stdout);
                            if (rrc == 0) {
                                char rl[64];

                                sprintf(rl, "Chat list reloaded (%d chats)",
                                        state->chat_count);
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status), rl);
                            } else {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Reload failed");
                            }
                            tg_gui_window_paint(state, &backend);
                        } else if (ud == (APTR)TG_MENU_OWNSCREEN) {
                            /* Flip own-screen mode and reopen: persist the new
                               flag now so the reopen (which reloads geometry)
                               honours it, then leave via the reopen path. */
                            want_own = !want_own;
                            tg_gui_window_save_geom(ctx.inner_w, ctx.inner_h,
                                                    (int)ctx.window->LeftEdge,
                                                    (int)ctx.window->TopEdge,
                                                    want_own);
                            done = 3; /* reopen (no AppIcon) */
                        } else if (ud == (APTR)TG_MENU_QUIT) {
                            done = 1;
                        }
                    }
                    if (item == 0) {
                        break; /* keyboard path: a single action */
                    }
                    mnum = item->NextSelect;
                }
            } else if (msg_class == IDCMP_NEWSIZE) {
                int first_resize;

                first_resize = !resize_pending;
                if (first_resize) {
                    tg_gui_log("resize: begin");
                    resize_pending = 1;
                    ctx.photo_resize_active = 1;
                }
                tg_gui_amiga_measure_geometry(&ctx);
                if (first_resize && ctx.bitmap_text_compat) {
                    tg_gui_amiga_resize_blank(&ctx);
                }
                /* A live size drag can queue many NEWSIZE/REFRESH pairs. Do not
                   free, allocate and repaint for every intermediate geometry:
                   drain the queue first, then rebuild once for the newest size
                   below, outside every BeginRefresh bracket. */
                resize_pending = 1;
                resize_settle_ticks = 1;
                ctx.photo_resize_active = 1;
            } else if (msg_class == IDCMP_REFRESHWINDOW) {
                /* BeginRefresh() already holds this window's layer locked for the
                   whole bracket, so no LockLayerRom here. With the buffer -- and
                   only while it still matches the current size -- copy it into the
                   exposed clip region (BltBitMapRastPort honours the ClipRects, so
                   just the damaged area is touched). Otherwise re-render raw,
                   except while the deferred resize rebuild below is pending. */
                BeginRefresh(ctx.window);
                tg_gui_amiga_measure_geometry(&ctx);
                if (!resize_pending && ctx.buf_ok && ctx.buf_bm != 0 &&
                    ctx.buf_w == ctx.inner_w && ctx.buf_h == ctx.inner_h) {
                    BltBitMapRastPort(ctx.buf_bm, 0, 0, ctx.rport,
                                      ctx.origin_x, ctx.origin_y,
                                      ctx.inner_w, ctx.inner_h, 0xC0);
                    (void)tg_gui_photo_direct_replay(
                        &ctx, 0, 0, ctx.inner_w, ctx.inner_h);
                } else if (!resize_pending) {
                    tg_gui_photo_frame_begin();
                    tg_gui_paint(state, &backend);
                } else {
                    /* Live/opaque resize implementations (notably AfA_OS) ask
                       for intermediate refreshes while the pointer is moving.
                       Show only the stable background during that burst; the
                       coalesced path below rebuilds the complete frame once. */
                    SetAPen(ctx.rport, ctx.pens[TG_GUI_PEN_WINDOW]);
                    RectFill(ctx.rport, ctx.origin_x, ctx.origin_y,
                             ctx.origin_x + ctx.inner_w - 1,
                             ctx.origin_y + ctx.inner_h - 1);
                }
                EndRefresh(ctx.window, TRUE);
                tg_gui_photo_direct_report(&ctx);
            } else if (msg_class == IDCMP_INTUITICKS) {
                if (state->mode != TG_GUI_MODE_CHAT) {
                    /* A login screen owns the keyboard: blink the caret (~2 Hz);
                       no network poll until the session opens. */
                    if (++caret_ticks >= 5) {
                        caret_ticks = 0;
                        state->cursor_on = !state->cursor_on;
                        /* Repaint ONLY the login input box, not the whole window
                           -- a full repaint twice a second was a visible refresh
                           on OS3. */
                        tg_gui_window_paint_caret(state, &backend);
                    }
                } else {
                    time_t now;

                    /* Self-heal the right-button trap from the current pointer
                       position, in case the MOUSEMOVE that should have released
                       it (over the sidebar/menu-bar area) was dropped by the
                       OS4 emulated mouse -- otherwise the sidebar right-click
                       had no menu. mouse_x/mouse_y hold this tick's position. */
                    tg_gui_window_track_rmbtrap(state, &ctx, (int)mouse_x,
                                                (int)mouse_y);

                    /* Blink the composer caret (~2 Hz) while typing. */
                    if ((state->composing || state->search_active) &&
                        ++caret_ticks >= 5) {
                        caret_ticks = 0;
                        state->cursor_on = !state->cursor_on;
                        /* Repaint ONLY the focused input strip (composer row or
                           the sidebar search box), not the whole window -- a full
                           repaint twice a second flickered on slow OS3 displays. */
                        tg_gui_window_paint_caret(state, &backend);
                    }

                    /* Live poll on the watch interval -- now runs even while the
                       composer is focused (keep-focus-after-send leaves
                       composing=1, and pausing the poll there silently stopped
                       reception + the "is typing" header). To keep typing smooth,
                       defer the (blocking) tick until the composer has been idle
                       for TG_GUI_COMPOSE_IDLE_POLL_SECONDS; skip it entirely when
                       a close/quit is already queued this drain. */
                    now = time(0);
                    if (!done &&
                        (state->composing ||
                         tg_gui_session_transfer_busy()) &&
                        now != (time_t)-1 &&
                        now >= last_receive_drain &&
                        (unsigned long)(now - last_receive_drain) >=
                            TG_GUI_COMPOSE_RECEIVE_SECONDS) {
                        last_receive_drain = now;
                        if (tg_gui_session_receive_pending(stdout)) {
                            session_dirty = 1;
                        }
                    }
                    /* (The online as-you-type debounce is gone: typing now
                       filters the LOCAL cache instantly; the network search
                       only runs from the explicit "Search Telegram..." row
                       or ENTER with no local match.) */
                    /* Effective interval: hold the conservative boot cadence until
                       the startup network burst has had WATCH_BOOT_GRACE seconds to
                       settle, then use the faster steady-state interval. */
                    {
                        unsigned long eff = watch_seconds;
                        if (watch_boot_grace &&
                            session_boot != (time_t)-1 && now != (time_t)-1 &&
                            (unsigned long)(now - session_boot) < watch_boot_grace) {
                            eff = watch_boot_seconds;
                        }
                        if (tg_gui_session_transfer_busy() &&
                            eff < TG_GUI_TRANSFER_POLL_SECONDS) {
                            /* Transfer pumping: the light drain above keeps
                               pushes flowing; the heavy tick can wait. */
                            eff = TG_GUI_TRANSFER_POLL_SECONDS;
                        }
                        effective_watch = eff;
                    }
                    if (!done && now != (time_t)-1 &&
                        (unsigned long)(now - last_session_poll) >=
                            effective_watch &&
                        (!state->composing ||
                         (unsigned long)(now - last_key_time) >=
                             TG_GUI_COMPOSE_IDLE_POLL_SECONDS)) {
                        last_session_poll = now;
                        if (tg_gui_session_tick(stdout)) {
                            session_dirty = 1;
                        }
                    }
                }
            } else if (msg_class == IDCMP_MOUSEBUTTONS &&
                       state->mode == TG_GUI_MODE_CHAT) {
                int hx;
                int hy;

                hx = (int)mouse_x - ctx.origin_x;
                hy = (int)mouse_y - ctx.origin_y;
                if (msg_code == MENUDOWN && !state->forward_pick_active) {
                    /* Right press with RMBTRAP held: open OUR context menu on
                       the bubble under the pointer. (Without the trap the
                       press never reaches us -- Intuition runs the menu bar.) */
                    int hit = tg_gui_hit_test(state, ctx.inner_w, ctx.inner_h,
                                              ctx.line_h, hx, hy);

                    if (hit >= TG_GUI_HIT_EMOJI_BASE) {
                        /* an emoji cell: insert it, keep the panel open */
                        state->emoji_sel = hit - TG_GUI_HIT_EMOJI_BASE;
                        (void)tg_gui_emoji_pick(state);
                        tg_gui_window_paint(state, &backend);
                    } else if (hit == TG_GUI_HIT_EMOJI_BUTTON) {
                        if (state->emoji_active) {
                            tg_gui_emoji_close(state);
                        } else {
                            tg_gui_emoji_open(state);
                        }
                        tg_gui_window_paint(state, &backend);
                    } else if (hit <= TG_GUI_HIT_MESSAGE_BASE) {
                        int mi = hit <= TG_GUI_HIT_PHOTO_BASE
                            ? TG_GUI_HIT_PHOTO_BASE - hit
                            : TG_GUI_HIT_MESSAGE_BASE - hit;

                        if (mi >= 0 && mi < state->message_count &&
                            !state->messages[mi].is_system &&
                            state->messages[mi].id != 0UL) {
                            state->ctx_visible = 1;
                            state->ctx_msg = mi;
                            state->ctx_x = hx;
                            state->ctx_y = hy;
                            state->ctx_hover = -1;
                            /* AFTER ctx_msg: the item list (and so the widest
                               label) depends on the clicked message. */
                            state->ctx_w =
                                tg_gui_context_menu_measure(state, &backend);
                            tg_gui_window_paint(state, &backend);
                        }
                    }
                } else if (msg_code == SELECTDOWN &&
                           (state->sel_press_armed = 0, 0)) {
                    /* never taken: clears a stale press latch before ANY other
                       SELECTDOWN branch runs (ctx menu, sidebar, gadgets...);
                       the bubble branch below re-arms it deliberately. */
                } else if (msg_code == SELECTDOWN && state->composing &&
                           state->mention_active &&
                           tg_gui_mention_click(state, &backend, hx, hy) >= 0) {
                    /* Click a candidate in the '@' popup: select it and insert,
                       exactly like ENTER/TAB on the keyboard-highlighted row. */
                    state->mention_sel =
                        tg_gui_mention_click(state, &backend, hx, hy);
                    tg_gui_window_mention_complete(state);
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == SELECTDOWN && state->ctx_visible) {
                    /* A left click while the context menu is open: run the item
                       under the pointer, or dismiss when the click is outside. */
                    int it = tg_gui_context_menu_hit(state, ctx.inner_w,
                                                     ctx.inner_h, ctx.line_h,
                                                     hx, hy);
                    int mi = state->ctx_msg;
                    const tg_gui_message *m =
                        (mi >= 0 && mi < state->message_count)
                            ? &state->messages[mi]
                            : 0;

                    state->ctx_visible = 0;
                    if (it == TG_GUI_CTX_REPLY && m != 0 && !m->is_system &&
                        m->id != 0UL) {
                        state->in_sel_active = 0;
                        state->reply_to_id = m->id;
                        tg_gui_window_copy(state->reply_sender,
                                           sizeof(state->reply_sender),
                                           m->sender);
                        tg_gui_window_copy(state->reply_snippet,
                                           sizeof(state->reply_snippet),
                                           m->text);
                        state->edit_to_id = 0UL;
                        state->search_active = 0;
                        state->composing = 1;
                        state->input_caret = (int)strlen(state->input);
                        state->cursor_on = 1;
                        caret_ticks = 0;
                        tg_gui_window_copy(state->status, sizeof(state->status),
                                           "Reply - ENTER sends, ESC cancels");
                    } else if (it == TG_GUI_CTX_EDIT && m != 0 &&
                               (state->in_sel_active = 0, 1) &&
                               (m->is_own ||
                                tg_gui_open_chat_is_self(state)) &&
                               m->id != 0UL) {
                        /* Edit mode: pre-fill the composer with the message text;
                           the next ENTER routes to editMessage (edit_to_id). */
                        state->edit_to_id = m->id;
                        state->reply_to_id = 0UL;
                        state->reply_sender[0] = '\0';
                        state->reply_snippet[0] = '\0';
                        tg_gui_window_copy(state->input, sizeof(state->input),
                                           m->text);
                        state->search_active = 0;
                        state->composing = 1;
                        state->input_caret = (int)strlen(state->input);
                        state->cursor_on = 1;
                        caret_ticks = 0;
                        tg_gui_window_copy(state->status, sizeof(state->status),
                                           "Editing - ENTER saves, ESC cancels");
                    } else if (it == TG_GUI_CTX_DELETE && m != 0 &&
                               (m->is_own ||
                                tg_gui_open_chat_is_self(state)) &&
                               m->id != 0UL) {
                        unsigned long del_id = m->id;

                        if (tg_gui_amiga_confirm_delete(ctx.window) != 0) {
                            (void)tg_gui_session_delete(del_id, stdout);
                        }
                    } else if (it == TG_GUI_CTX_DOWNLOAD && m != 0 &&
                               m->has_document && m->id != 0UL) {
                        unsigned long dl_id = m->id;
                        int drc;

                        /* 0.0.8 1b: ARM the download and return to the loop;
                           the pump below moves one chunk per turn, so the
                           window keeps living (type, switch chats, receive)
                           while the file streams in. */
                        if (tg_gui_session_transfer_busy()) {
                            tg_gui_window_copy(state->status,
                                               sizeof(state->status),
                                               "A transfer is already running");
                            tg_gui_window_paint(state, &backend);
                        } else if ((drc = tg_gui_session_transfer_start_download(
                                        dl_id, stdout)) == 0) {
                            tg_gui_window_copy(
                                state->status, sizeof(state->status),
                                "Downloading... (ESC cancels)");
                            tg_gui_window_paint(state, &backend);
                        } else {
                            /* Failed before the first chunk (not in cache,
                               foreign DC, downloads/ not writable): the same
                               final lines as ever, reason via the session. */
                            tg_gui_window_transfer_finished(
                                state, &backend, 1, drc,
                                tg_gui_session_last_transfer_error());
                        }
                    } else if (it == TG_GUI_CTX_SAVE_PHOTO && m != 0 &&
                               m->has_photo &&
                               (m->photo_id_hi != 0UL ||
                                m->photo_id_lo != 0UL)) {
                        tg_gui_photo_save_begin(
                            state, ctx.window, &backend, &photo_save,
                            m->photo_id_hi, m->photo_id_lo);
                    } else if (it == TG_GUI_CTX_COPY && m != 0 &&
                               m->text[0] != '\0') {
                        static char ctxsel[TG_GUI_MSG_TEXT_MAX];
                        const char *src = m->text;

                        /* A dragged selection in THIS message wins over the
                           whole text. */
                        if (state->sel_active && state->sel_msg == mi &&
                            tg_gui_selection_get(state, ctxsel,
                                                 sizeof(ctxsel))) {
                            src = ctxsel;
                        }
                        state->selected_msg = mi; /* keep the row highlighted */
                        tg_gui_window_copy(state->status,
                                           sizeof(state->status),
                                           tg_gui_clip_write_text(src)
                                               ? "Copied to clipboard"
                                               : "Copy failed");
                        tg_gui_window_paint(state, &backend);
                    } else if (it == TG_GUI_CTX_FORWARD_SAVED && m != 0 &&
                               !m->is_system && m->id != 0UL) {
                        unsigned long forward_id = m->id;
                        int frc = tg_gui_session_forward(
                            forward_id, TG_GUI_SAVED_PEER_INDEX, stdout);

                        if (frc == 0) {
                            tg_gui_window_copy(
                                state->status, sizeof(state->status),
                                "Forwarded to Saved Messages");
                        } else {
                            const char *why =
                                tg_gui_session_last_action_error();
                            if (why != 0 && why[0] != '\0') {
                                sprintf(state->status,
                                        "Forward failed: %.30s", why);
                            } else {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    "Could not forward message");
                            }
                        }
                        tg_gui_window_paint(state, &backend);
                    } else if (it == TG_GUI_CTX_FORWARD_TO && m != 0 &&
                               !m->is_system && m->id != 0UL) {
                        tg_gui_window_begin_forward_pick(state, &backend,
                                                         m->id);
                    } else if (it == TG_GUI_CTX_SENDFILE) {
                        /* Chat-level action (not tied to the clicked message):
                           send a file to the open chat, same as the menubar
                           "Send file..." item. */
                        tg_gui_window_send_file(state, ctx.window, &backend);
                    } else if (it == TG_GUI_CTX_SENDPHOTO) {
                        /* Its photo twin, so both ways of sending sit where
                           the conversation is instead of only in the menus. */
                        tg_gui_window_send_photo(state, ctx.window, &backend);
                    }
                    tg_gui_window_paint(state, &backend);
                } else if (msg_code == SELECTUP) {
                    state->in_drag_armed = 0;
                    if (state->sel_press_armed) {
                        state->sel_press_armed = 0;
                        if (!state->sel_active &&
                            tg_gui_session_is_open() && !state->in_search &&
                            !state->forward_pick_active) {
                            /* Plain click (never dragged). A single click just
                               SELECTS/highlights the bubble; a second click on
                               the SAME bubble within the system double-click
                               interval opens the reply. The press-time ID must
                               match, so a transcript shift between press and
                               release cannot aim the gesture at another
                               message. */
                            int mi = state->sel_press_msg;

                            if (mi >= 0 && mi < state->message_count) {
                                const tg_gui_message *m = &state->messages[mi];

                                if (!m->is_system && m->id != 0UL &&
                                    m->id == state->sel_press_id) {
                                    char clicked_url[256];
                                    int dbl = (dbl_last_id != 0UL &&
                                               dbl_last_id == m->id &&
                                               DoubleClick(dbl_last_secs,
                                                           dbl_last_micros,
                                                           dbl_press_secs,
                                                           dbl_press_micros));

                                    /* A click ON A LINK beats select/reply:
                                       open it in the browser, or copy it to
                                       the clipboard when no OpenURL/URLOpen
                                       command is around (0.0.8). */
                                    if (state->sel_press_char >= 0 &&
                                        tg_gui_url_at(m,
                                                      state->sel_press_char,
                                                      clicked_url,
                                                      sizeof(clicked_url))) {
                                        dbl_last_id = 0UL; /* never a reply */
                                        if (tg_platform_open_url(
                                                clicked_url) == 0) {
                                            tg_gui_window_copy(
                                                state->status,
                                                sizeof(state->status),
                                                "Opening URL in the browser...");
                                        } else if (tg_gui_clip_write_text(
                                                       clicked_url)) {
                                            tg_gui_window_copy(
                                                state->status,
                                                sizeof(state->status),
                                                "URL copied (no OpenURL command)");
                                        } else {
                                            tg_gui_window_copy(
                                                state->status,
                                                sizeof(state->status),
                                                "Could not open the URL");
                                        }
                                        tg_gui_window_paint(state, &backend);
                                        continue;
                                    }
                                    if (dbl) {
                                        /* Double-click = reply to this bubble. */
                                        dbl_last_id = 0UL; /* consume the pair */
                                        state->in_sel_active = 0;
                                        state->selected_msg = mi;
                                        state->reply_to_id = m->id;
                                        tg_gui_window_copy(
                                            state->reply_sender,
                                            sizeof(state->reply_sender),
                                            m->sender);
                                        tg_gui_window_copy(
                                            state->reply_snippet,
                                            sizeof(state->reply_snippet),
                                            m->text);
                                        state->search_active = 0;
                                        state->composing = 1;
                                        state->input_caret =
                                            (int)strlen(state->input);
                                        state->cursor_on = 1;
                                        caret_ticks = 0;
                                        tg_gui_window_copy(
                                            state->status,
                                            sizeof(state->status),
                                            "Reply - ENTER sends, ESC cancels");
                                        tg_gui_window_paint(state, &backend);
                                    } else {
                                        /* Single click = select/highlight;
                                           record it so the next click can pair
                                           into a double-click. */
                                        dbl_last_secs = dbl_press_secs;
                                        dbl_last_micros = dbl_press_micros;
                                        dbl_last_id = m->id;
                                        state->selected_msg = mi;
                                        tg_gui_window_copy(
                                            state->status,
                                            sizeof(state->status),
                                            "Selected - double-click to reply, "
                                            "A+C copies");
                                        tg_gui_window_paint(state, &backend);
                                    }
                                }
                            }
                        } else {
                            /* Drag finished: keep the text selection, tell how
                               to use it. Not a click -> break any double-click
                               pairing. */
                            dbl_last_id = 0UL;
                            tg_gui_window_copy(state->status,
                                               sizeof(state->status),
                                               "Selected - A+C copies");
                            tg_gui_window_paint(state, &backend);
                        }
                    }
                    state->sb_drag = 0;
                    if (state->drag_src >= 0) {
                        if (!state->drag_active) {
                            /* CLICK (never crossed the threshold): open the chat,
                               same logic that used to run on SELECTDOWN. */
                            int hit = state->drag_src;

                            state->search_active = 0;
                            if (state->composing) {
                                state->composing = 0;
                                state->input[0] = '\0';
                                state->input_caret = 0;
                                state->history_pos = -1;
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Live - F1-F10 chats, Q quits");
                            }
                            if (hit >= 0 && hit < state->chat_count &&
                                hit != state->selected_chat) {
                                tg_gui_window_open_selection(state, hit, &backend);
                            } else {
                                tg_gui_window_paint(state, &backend);
                            }
                        } else {
                            /* DRAG: reorder. Map the cursor to an insert-before
                               target, convert to a final 0-based destination row,
                               persist + reproject if it actually moves. */
                            int target = tg_gui_chat_drop_target(
                                state, ctx.line_h, state->drag_cur_y);
                            int dest = (target > state->drag_src) ? (target - 1)
                                                                  : target;

                            if (dest < 0) {
                                dest = 0;
                            }
                            if (dest >= state->chat_count) {
                                dest = state->chat_count - 1;
                            }
                            if (dest != state->drag_src &&
                                state->chats[state->drag_src].index !=
                                    TG_GUI_SAVED_PEER_INDEX &&
                                state->chats[dest].index !=
                                    TG_GUI_SAVED_PEER_INDEX) {
                                /* Hidden rows are absent from the sidebar, so
                                   use each visible row's stable cache index. */
                                (void)tg_gui_session_reorder_chat(
                                    state->chats[state->drag_src].index,
                                    state->chats[dest].index, stdout);
                            }
                            tg_gui_window_paint(state, &backend);
                        }
                        state->drag_src = -1;
                        state->drag_active = 0;
                    }
                } else if (msg_code == SELECTDOWN && state->jb_w > 0 &&
                           hx >= state->jb_x &&
                           hx < state->jb_x + state->jb_w &&
                           hy >= state->jb_y &&
                           hy < state->jb_y + state->jb_h) {
                    /* Floating scroll-to-bottom button: jump to the true newest.
                       jb_w == 0 when the painter did not draw it this frame, so a
                       click in that area then falls through to the normal hits. */
                    tg_gui_window_jump_to_bottom(state, &backend,
                                                 &older_exhausted, &older_cooldown);
                } else if (msg_code == SELECTDOWN && state->sb_tr_max > 0 &&
                           hx >= state->sb_tr_x &&
                           hx < state->sb_tr_x + TG_GUI_SCROLLBAR_W &&
                           hy >= state->sb_tr_ty &&
                           hy < state->sb_tr_ty + state->sb_tr_th) {
                    if (hy >= state->sb_tr_ky &&
                        hy < state->sb_tr_ky + state->sb_tr_kh) {
                        state->sb_drag = 2;
                        state->sb_grab_dy = hy - state->sb_tr_ky;
                    } else {
                        int page = state->sb_tr_th;

                        if (page < 1) {
                            page = 1;
                        }
                        if (hy < state->sb_tr_ky) {
                            state->transcript_scroll += page;
                            want_older = 1;
                        } else {
                            state->transcript_scroll -= page;
                            if (state->transcript_scroll < 0) {
                                state->transcript_scroll = 0;
                            }
                        }
                        scroll_dirty = 1;
                    }
                } else if (msg_code == SELECTDOWN && state->sb_list_max > 0 &&
                           hx >= state->sb_list_x &&
                           hx < state->sb_list_x + TG_GUI_SCROLLBAR_W &&
                           hy >= state->sb_list_ty &&
                           hy < state->sb_list_ty + state->sb_list_th) {
                    if (hy >= state->sb_list_ky &&
                        hy < state->sb_list_ky + state->sb_list_kh) {
                        state->sb_drag = 1;
                        state->sb_grab_dy = hy - state->sb_list_ky;
                    } else {
                        int page = state->chat_count - state->sb_list_max;

                        if (page < 1) {
                            page = 1;
                        }
                        if (hy < state->sb_list_ky) {
                            state->chat_scroll -= page;
                            if (state->chat_scroll < 0) {
                                state->chat_scroll = 0;
                            }
                        } else {
                            state->chat_scroll += page;
                        }
                        scroll_dirty = 1;
                    }
                } else if (msg_code == SELECTDOWN) {
                    int hit;

                    if (picked_secs != 0UL &&
                        DoubleClick(picked_secs, picked_micros, msg_secs,
                                    msg_micros)) {
                        /* Second half of a double click on a search result:
                           the list under the pointer is a different one now,
                           so acting on it would open the wrong chat. */
                        picked_secs = 0UL;
                        continue;
                    }
                    picked_secs = 0UL;
                    hit = tg_gui_hit_test(state, ctx.inner_w, ctx.inner_h,
                                          ctx.line_h, hx, hy);
                    if (hit <= TG_GUI_HIT_PHOTO_BASE) {
                        int mi = TG_GUI_HIT_PHOTO_BASE - hit;

                        if (mi >= 0 && mi < state->message_count &&
                            state->messages[mi].has_photo) {
                            if (tg_gui_photo_viewer_show(
                                    &viewer, &ctx, &state->messages[mi]) == 0) {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Photo viewer open");
                            } else {
                                tg_gui_window_copy(state->status,
                                                   sizeof(state->status),
                                                   "Could not open that photo");
                            }
                            tg_gui_window_paint(state, &backend);
                        }
                        continue;
                    } else if (hit >= 0 && state->in_search) {
                        /* Picker: a normal search opens the result; forwarding
                           caches it as a destination without opening/unhiding. */
                        if (state->forward_pick_active) {
                            tg_gui_window_forward_online_result(state, &backend,
                                                                hit);
                        } else {
                            state->in_search = 0;
                            state->search_active = 0;
                            state->search_query[0] = '\0';
                            (void)tg_gui_session_search_open_result(hit, stdout);
                            tg_gui_window_copy(
                                state->status, sizeof(state->status),
                                "Live - F1-F10 chats, Q quits");
                            tg_gui_window_paint(state, &backend);
                        }
                        picked_secs = msg_secs;   /* swallow the second click */
                        picked_micros = msg_micros;
                    } else if (hit >= 0 && state->in_filter &&
                               hit < state->chat_count) {
                        /* Local filter: a click opens the row right away (no
                           reorder drag on a filtered list -- the row order
                           is not the cache order). Index 0 = go online. */
                        if (state->chats[hit].index == 0UL) {
                            tg_gui_window_run_search(state, &backend, 1);
                        } else if (state->forward_pick_active) {
                            tg_gui_window_forward_to_index(
                                state, &backend, state->chats[hit].index,
                                state->chats[hit].name);
                            picked_secs = msg_secs;
                            picked_micros = msg_micros;
                        } else {
                            tg_gui_window_open_by_index(
                                state, &backend, state->chats[hit].index);
                            picked_secs = msg_secs; /* swallow the second click */
                            picked_micros = msg_micros;
                            tg_gui_window_copy(state->status,
                                               sizeof(state->status),
                                               "Live - F1-F10 chats, Q quits");
                            tg_gui_window_paint(state, &backend);
                        }
                    } else if (hit >= 0) {
                        /* Press on a chat row: ARM a reorder drag. The open is
                           deferred to SELECTUP -- a press that never crosses the
                           drag threshold opens the chat (click), a press that does
                           reorders the list (drag). drag_src doubles as the flag. */
                        state->drag_src = hit;
                        state->drag_active = 0;
                        state->drag_press_y = hy;
                        state->drag_cur_y = hy;
                    } else if (hit == TG_GUI_HIT_SEARCH &&
                               tg_gui_session_is_open()) {
                        /* Click the sidebar search box to focus it for typing;
                           F8: the caret lands where you clicked. */
                        int sc;

                        state->composing = 0;
                        state->search_active = 1;
                        last_key_time = time(0);
                        state->cursor_on = 1;
                        caret_ticks = 0;
                        sc = tg_gui_search_click_caret(state, &backend, hx, hy);
                        state->search_caret =
                            (sc >= 0) ? sc : (int)strlen(state->search_query);
                        state->nav_chat = -1; /* the box owns the arrows now */
                        tg_gui_window_copy(
                            state->status, sizeof(state->status),
                            "Search: type to filter your chats (arrows+ENTER)");
                        /* Surfaces the "Browse all chats..." top row right away
                           when the box opens empty (it paints either way). */
                        tg_gui_window_filter_chats(state, &backend);
                    } else if (hit == TG_GUI_HIT_SEND && state->composing) {
                        state->in_sel_active = 0; /* input is consumed below */
                        if (state->input[0] != '\0') {
                            if (state->edit_to_id != 0UL) {
                                (void)tg_gui_session_edit(state->input,
                                                          state->edit_to_id,
                                                          stdout);
                                state->edit_to_id = 0UL;
                            } else if (tg_gui_session_send(state->input,
                                                           state->reply_to_id,
                                                           stdout) == 0) {
                                tg_gui_history_add(state, state->input);
                                state->reply_to_id = 0UL; /* clear on success */
                                state->reply_sender[0] = '\0';
                                state->reply_snippet[0] = '\0';
                                tg_gui_window_jump_to_bottom(state, &backend,
                                                             &older_exhausted,
                                                             &older_cooldown);
                            }
                            state->input[0] = '\0';
                        }
                        state->input_caret = 0;
                        state->history_pos = -1;
                        state->history_draft[0] = '\0';
                        /* Keep focus in the composer so the next message can be
                           typed without re-clicking; re-prime the caret blink. */
                        state->composing = 1;
                        state->cursor_on = 1;
                        caret_ticks = 0;
                        tg_gui_window_copy(state->status, sizeof(state->status),
                                       "Type - ENTER sends, ESC cancels");
                        tg_gui_window_paint(state, &backend);
                    } else if (hit == TG_GUI_HIT_INPUT && state->composing) {
                        /* F8: click places the caret in the composer text.
                           Clicking the composer also drops any transcript
                           message highlight (an active reply keeps its own
                           header). */
                        int cc = tg_gui_input_click_caret(state, &backend, hx,
                                                          hy);
                        int had_sel = (state->selected_msg >= 0);

                        state->selected_msg = -1;
                        dbl_last_id = 0UL;
                        if (cc >= 0) {
                            state->in_sel_active = 0;
                            state->in_drag_armed = 1;
                            state->in_drag_anchor = cc;
                            state->input_caret = cc;
                            state->cursor_on = 1;
                            caret_ticks = 0;
                            tg_gui_window_mention_refresh(state);
                            tg_gui_window_paint(state, &backend);
                        } else if (had_sel) {
                            tg_gui_window_paint(state, &backend);
                        }
                    } else if ((hit == TG_GUI_HIT_INPUT ||
                                hit == TG_GUI_HIT_SEND) &&
                               !state->composing && tg_gui_session_is_open()) {
                        /* Click the input field (or Send) to start composing --
                           leave the search box so only one caret is focused. */
                        state->search_active = 0;
                        state->search_query[0] = '\0';
                        if (state->forward_pick_active) {
                            tg_gui_window_forward_restore(state);
                        } else if (state->in_search || state->in_filter) {
                            /* Abandon the search picker/filter, restore chats. */
                            state->in_search = 0;
                            state->in_filter = 0;
                            tg_gui_session_refresh_chats();
                        }
                        state->composing = 1;
                        state->in_sel_active = 0; /* fresh focus, no ghosts */
                        state->selected_msg = -1; /* clicking to type deselects */
                        dbl_last_id = 0UL;
                        state->input_caret = (int)strlen(state->input);
                        if (hit == TG_GUI_HIT_INPUT) { /* F8: caret at click */
                            int cc = tg_gui_input_click_caret(state, &backend,
                                                              hx, hy);

                            if (cc >= 0) {
                                state->in_sel_active = 0;
                                state->in_drag_armed = 1;
                                state->in_drag_anchor = cc;
                                state->input_caret = cc;
                            }
                        }
                        state->cursor_on = 1;
                        caret_ticks = 0;
                        tg_gui_window_copy(state->status, sizeof(state->status),
                               "Type - ENTER sends, ESC cancels");
                        tg_gui_window_paint(state, &backend);
                    } else if (hit == TG_GUI_HIT_REPLY_CANCEL) {
                        /* The composer's reply header "X": drop the reply target
                           (composer shrinks, transcript grows back). */
                        state->reply_to_id = 0UL;
                        state->reply_sender[0] = '\0';
                        state->reply_snippet[0] = '\0';
                        tg_gui_window_paint(state, &backend);
                    } else if (hit <= TG_GUI_HIT_MESSAGE_BASE &&
                               hit > TG_GUI_HIT_PHOTO_BASE &&
                               tg_gui_session_is_open() && !state->in_search &&
                               !state->forward_pick_active) {
                        /* Press on a bubble: LATCH it. A drag past the
                           threshold becomes a text selection; a click that
                           never drags keeps the old gesture (reply), executed
                           on SELECTUP -- same defer pattern as the sidebar. */
                        int mi = TG_GUI_HIT_MESSAGE_BASE - hit;

                        if (mi >= 0 && mi < state->message_count) {
                            const tg_gui_message *m = &state->messages[mi];

                            if (!m->is_system && m->id != 0UL) {
                                int repaint = state->sel_active;

                                state->sel_active = 0; /* new press resets */
                                state->sel_press_armed = 1;
                                state->sel_press_msg = mi;
                                state->sel_press_id = m->id;
                                state->sel_press_gen = state->msg_gen;
                                state->sel_press_x = hx;
                                state->sel_press_y = hy;
                                /* Remember this press's time; SELECTUP compares
                                   it with the previous click to spot a double-
                                   click (= reply). */
                                dbl_press_secs = msg_secs;
                                dbl_press_micros = msg_micros;
                                state->sel_press_char =
                                    tg_gui_transcript_char_at(state, &backend,
                                                              ctx.line_h, mi,
                                                              hx, hy);
                                if (repaint) {
                                    tg_gui_window_paint(state, &backend);
                                }
                            }
                        }
                    }
                }
            } else if (msg_class == IDCMP_MOUSEMOVE) {
                if (state->mode == TG_GUI_MODE_CHAT && state->in_drag_armed &&
                    state->composing) {
                    /* Drag inside the input box: grow the composer selection
                       from the press anchor to the char under the pointer. */
                    int hx = (int)mouse_x - ctx.origin_x;
                    int hy = (int)mouse_y - ctx.origin_y;
                    int cc = tg_gui_input_click_caret(state, &backend, hx, hy);

                    if (cc >= 0) {
                        if (!state->in_sel_active &&
                            cc != state->in_drag_anchor) {
                            state->in_sel_active = 1;
                            state->in_sel_anchor = state->in_drag_anchor;
                        }
                        if (state->in_sel_active &&
                            cc == state->in_sel_anchor &&
                            cc == state->input_caret) {
                            state->in_sel_active = 0; /* collapsed on anchor */
                            tg_gui_window_paint(state, &backend);
                        } else if (state->in_sel_active &&
                            cc != state->input_caret) {
                            state->input_caret = cc;
                            if (cc == state->in_sel_anchor) {
                                state->in_sel_active = 0;
                            }
                            tg_gui_window_paint(state, &backend);
                        }
                    }
                }
                if (state->mode == TG_GUI_MODE_CHAT && state->sel_press_armed) {
                    /* Latched press: past a small threshold it becomes a text
                       selection anchored at the press char; every further move
                       extends it (clamped inside the SAME message by the
                       char-at helper). */
                    int hx = (int)mouse_x - ctx.origin_x;
                    int hy = (int)mouse_y - ctx.origin_y;

                    if (!state->sel_active && state->sel_press_char >= 0 &&
                        state->msg_gen == state->sel_press_gen) {
                        int dx = hx - state->sel_press_x;
                        int dy = hy - state->sel_press_y;

                        if (dx > 2 || dx < -2 || dy > 2 || dy < -2) {
                            state->sel_active = 1;
                            state->sel_msg = state->sel_press_msg;
                            state->sel_a = state->sel_press_char;
                            state->sel_b = state->sel_press_char;
                            state->sel_gen_snap = state->msg_gen;
                        }
                    }
                    if (state->sel_active) {
                        long c = tg_gui_transcript_char_at(
                            state, &backend, ctx.line_h, state->sel_msg, hx,
                            hy);

                        if (c >= 0 && c != state->sel_b) {
                            state->sel_b = c;
                            tg_gui_window_paint(state, &backend);
                        }
                    }
                }
                /* Right-button trap follows the pointer: claimed over a real
                   bubble (our context menu), released elsewhere (menu bar).
                   The INTUITICKS handler runs the same check so a dropped move
                   cannot leave it stuck (see tg_gui_window_track_rmbtrap). */
                tg_gui_window_track_rmbtrap(state, &ctx, (int)mouse_x,
                                            (int)mouse_y);
                /* Context-menu hover: highlight the item under the pointer so the
                   user sees which of Reply/Edit/Delete the click will pick.
                   REPORTMOUSE floods moves, so repaint ONLY when the highlighted
                   item actually changes (crossing an item boundary). */
                if (state->ctx_visible) {
                    int hx = (int)mouse_x - ctx.origin_x;
                    int hy = (int)mouse_y - ctx.origin_y;
                    int hv = tg_gui_context_menu_index(state, ctx.inner_w,
                                                       ctx.inner_h, ctx.line_h,
                                                       hx, hy);

                    if (hv != state->ctx_hover) {
                        state->ctx_hover = hv;
                        tg_gui_window_paint(state, &backend);
                    }
                }
                /* Scrollbar knob drag: Intuition reports moves while a button is
                   held. Map the cursor to a scroll offset; the painter re-clamps
                   and redraws the knob to match. */
                if (state->sb_drag != 0) {
                    int hy;
                    int nky;
                    int span;

                    hy = (int)mouse_y - ctx.origin_y;
                    if (state->sb_drag == 2) {
                        span = state->sb_tr_th - state->sb_tr_kh;
                        nky = hy - state->sb_grab_dy;
                        if (nky < state->sb_tr_ty) {
                            nky = state->sb_tr_ty;
                        }
                        if (nky > state->sb_tr_ty + span) {
                            nky = state->sb_tr_ty + span;
                        }
                        if (span > 0) {
                            int off_top;

                            off_top = (nky - state->sb_tr_ty) *
                                      state->sb_tr_max / span;
                            state->transcript_scroll =
                                state->sb_tr_max - off_top;
                            if (state->transcript_scroll < 0) {
                                state->transcript_scroll = 0;
                            }
                            /* Dragged the knob to the very top -> page older
                               (the handler gates on transcript_scroll >= max). */
                            want_older = 1;
                        }
                    } else {
                        span = state->sb_list_th - state->sb_list_kh;
                        nky = hy - state->sb_grab_dy;
                        if (nky < state->sb_list_ty) {
                            nky = state->sb_list_ty;
                        }
                        if (nky > state->sb_list_ty + span) {
                            nky = state->sb_list_ty + span;
                        }
                        if (span > 0) {
                            state->chat_scroll = (nky - state->sb_list_ty) *
                                                 state->sb_list_max / span;
                        }
                    }
                    scroll_dirty = 1;
                } else if (state->drag_src >= 0) {
                    /* Row-reorder drag: promote once the gesture passes the
                       click/drag threshold (row_h/2), then track the cursor so the
                       painter redraws the insertion line. Gated on drag_src>=0 so
                       idle pointer motion (REPORTMOUSE is on) never reorders. */
                    int hy = (int)mouse_y - ctx.origin_y;
                    int thresh = ((2 * ctx.line_h) + 12) / 2;

                    state->drag_cur_y = hy;
                    if (!state->drag_active) {
                        int dy = hy - state->drag_press_y;

                        if (dy < 0) {
                            dy = -dy;
                        }
                        if (dy >= thresh) {
                            state->drag_active = 1;
                        }
                    }
                    if (state->drag_active) {
                        scroll_dirty = 1;
                    }
                }
            }
        }
        if (resize_pending && !done && photo_tick && !interactive_event &&
            !tg_gui_window_user_events_pending(&ctx, &viewer)) {
            if (resize_settle_ticks > 0) {
                resize_settle_ticks -= 1;
            }
        }
        if (resize_pending && !done && resize_settle_ticks <= 0) {
            tg_gui_log("resize: rebuild begin");
            tg_gui_amiga_measure_geometry(&ctx);
            tg_gui_amiga_buffer_alloc(&ctx);
            /* The first quiet tick marks release of the live-resize burst.
               Re-enable replay before this one final composition, then keep
               network/decode work out of the same turn. */
            ctx.photo_resize_active = 0;
            photo_resume_turn = 1;
            tg_gui_log("resize: repaint begin");
            tg_gui_window_paint(state, &backend);
            WaitBlit();
            tg_gui_log("resize: repaint done");
            /* NEWSIZE/REFRESH bursts can leave themed AfA/RTG borders with an
               exposed strip even though the client area is current. Ask
               Intuition to redraw its frame once, after the final geometry. */
            RefreshWindowFrame(ctx.window);
            resize_pending = 0;
            tg_gui_log("resize: end");
        }
        /* 0.0.8 punto 1e: Workbench drops. An icon dropped on the window
           arms an upload to the open chat, exactly like Send file... did;
           the pump below then moves it one part per turn. */
        {
            char dropped[256];

            if (tg_platform_console_drop_poll(dropped, sizeof(dropped)) &&
                dropped[0] != '\0') {
                if (state->mode != TG_GUI_MODE_CHAT ||
                    !tg_gui_session_is_open() || state->chat_count <= 0) {
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       "Open a chat first, then drop the file");
                    tg_gui_window_paint(state, &backend);
                } else if (tg_gui_session_transfer_busy()) {
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       "A transfer is already running");
                    tg_gui_window_paint(state, &backend);
                } else {
                    int urc;
                    int jpeg_mode;
                    int as_photo;
                    const char *dname = dropped;
                    const char *dp;

                    /* Name the dropped file the moment it lands: a drop on
                       Workbench gives no feedback of its own, so without
                       this the upload started silently and the gesture
                       looked ignored. */
                    for (dp = dropped; *dp != '\0'; ++dp) {
                        if (*dp == '/' || *dp == ':') {
                            dname = dp + 1;
                        }
                    }
                    if (tg_gui_window_path_is_jpeg(dropped)) {
                        char dropcap[512];

                        jpeg_mode = tg_gui_window_send_photo_dialog(
                            state, &ctx, ctx.window, dropped, dropcap,
                            sizeof(dropcap));
                        if (jpeg_mode != 0) {
                            as_photo = jpeg_mode == 1;
                            urc = as_photo
                                ? tg_gui_session_transfer_start_photo(
                                      dropped, dropcap, stdout)
                                : tg_gui_session_transfer_start_upload(
                                      dropped, dropcap, stdout);
                            if (urc == 0) {
                                state->input[0] = '\0';
                                state->input_caret = 0;
                            }
                        }
                    } else {
                        jpeg_mode = 2;
                        as_photo = 0;
                        urc = tg_gui_session_transfer_start_upload(dropped, 0,
                                                                   stdout);
                    }
                    if (jpeg_mode == 0) {
                        tg_gui_window_copy(state->status,
                                           sizeof(state->status),
                                           "Send cancelled");
                        tg_gui_window_paint(state, &backend);
                    } else {
                        if (urc == 0) {
                            /* The progress line below carries the name for the
                               whole transfer -- a one-off "Sending X..." here
                               was overwritten by the first pumped step a
                               millisecond later, so the drop still looked
                               ignored (field report). */
                            tg_gui_window_set_transfer_name(dname);
                            if (as_photo &&
                                tg_gui_session_transfer_photo_fallback()) {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    "Photo over 10 MiB; sending as file");
                            } else {
                                tg_gui_window_copy(
                                    state->status, sizeof(state->status),
                                    as_photo ? "Sending photo... (ESC cancels)"
                                             : "Sending... (ESC cancels)");
                            }
                            tg_gui_window_paint(state, &backend);
                        } else {
                            /* Same final lines the picker path shows on a
                               failed start (unreadable, too big, empty...). */
                            tg_gui_window_transfer_finished(
                                state, &backend, 2, urc, "");
                        }
                    }
                }
            }
        }
        /* 0.0.8 punto 1b: transfer pump. One bounded step (a single getFile
           chunk or saveFilePart) per loop turn; the Wait() above is skipped
           while a transfer is active, so typing, chat switches and the live
           tick below all interleave with the chunks. The status line tracks
           the percentage (repaint only when the line actually changes, which
           also restores it after a tick overwrote it). */
        if (tg_gui_session_transfer_busy()) {
            int tdir = tg_gui_session_transfer_busy();
            unsigned long tdone = 0UL;
            unsigned long ttotal = 0UL;

            if (tg_gui_session_transfer_step(&tdone, &ttotal)) {
                /* Percentage WITHOUT overflowing 32 bits: bytes*100 wraps
                   past 42.9 MB on every 32-bit lane, which sent the figure
                   back to 0 mid-file and climbing again (seen on an 82 MB
                   download). Below that the exact form keeps full
                   precision; above it, divide first. */
                unsigned long percent =
                    (ttotal == 0UL) ? 0UL
                    : (tdone <= 42949672UL) ? (tdone * 100UL) / ttotal
                                            : tdone / (ttotal / 100UL);
                char tline[96];
                time_t xnow = time(0);

                if (percent > 100UL) {
                    percent = 100UL; /* size meta can undercount */
                }
                /* Transfer rate over a rolling window: start the window on
                   the first pumped step, then recompute every 3s so the
                   figure is steady instead of jittering per chunk. */
                if (xfer_mark == (time_t)0 && xnow != (time_t)-1) {
                    xfer_mark = xnow;
                    xfer_bytes = tg_gui_session_transfer_bytes();
                } else if (xnow != (time_t)-1 && xnow > xfer_mark &&
                           (unsigned long)(xnow - xfer_mark) >= 3UL) {
                    unsigned long now_bytes =
                        tg_gui_session_transfer_bytes();

                    if (now_bytes > xfer_bytes) {
                        xfer_kbs = (now_bytes - xfer_bytes) /
                                   ((unsigned long)(xnow - xfer_mark) *
                                    1024UL);
                    }
                    xfer_mark = xnow;
                    xfer_bytes = now_bytes;
                }
                /* status is 48 bytes: keep the line short enough that the
                   rate always fits (the hint loses its "close or"). */
                if (tdir == 2 && tg_gui_xfer_name[0] != '\0') {
                    /* Uploads name the file: that IS the drop feedback, and
                       it stays up for the whole transfer. */
                    if (xfer_kbs > 0UL) {
                        sprintf(tline, "Sending %.20s %lu%% %lu KB/s",
                                tg_gui_xfer_name, percent, xfer_kbs);
                    } else {
                        sprintf(tline, "Sending %.20s %lu%% (ESC cancels)",
                                tg_gui_xfer_name, percent);
                    }
                } else if (xfer_kbs > 0UL) {
                    sprintf(tline, "%s %lu%% %lu KB/s (ESC cancels)",
                            tdir == 2 ? "Uploading" : "Downloading", percent,
                            xfer_kbs);
                } else {
                    sprintf(tline, "%s %lu%% (ESC cancels)",
                            tdir == 2 ? "Uploading" : "Downloading", percent);
                }
                if (strcmp(state->status, tline) != 0) {
                    tg_gui_window_copy(state->status, sizeof(state->status),
                                       tline);
                    tg_gui_window_paint(state, &backend);
                }
            } else {
                char saved[160];
                int trc = tg_gui_session_transfer_end(saved, sizeof(saved));

                xfer_mark = (time_t)0; /* next transfer starts a new window */
                xfer_kbs = 0UL;
                tg_gui_xfer_name[0] = '\0';

                tg_gui_window_transfer_finished(state, &backend, tdir, trc,
                                                saved);
                if (tdir == 2 && trc == 0) {
                    session_dirty = 1; /* the poll shows the sent file row */
                }
            }
        }
        /* Re-offer visible placeholders before deciding whether work exists.
           This heals a transient network failure without waiting for an
           unrelated repaint. m68k keeps its conservative input deferral;
           faster targets run a full bounded slice immediately even under a
           continuous VNC pointer stream. */
        if (photo_tick) {
            int pending;
            int events_pending;
            int reason;

            tg_gui_photo_reoffer_visible();
            /* Stripped thumbnails are the cheap pass zero, not part of the
               serialized network/JPEG-quality pipe. Drain every visible one
               before choosing whether this tick may spend a background turn. */
            if (!resize_pending && !ctx.photo_resize_active) {
                if (tg_gui_photo_preview_drain_visible(&ctx)) {
                    /* Publish the whole preview tier before a cache/network
                       step can block this event-loop turn. The repaint rebuilds
                       the request list for the quality tier below. */
                    tg_gui_window_paint(state, &backend);
                }
                if (tg_gui_photo_viewer_prepare_preview(&viewer) &&
                    viewer.ctx.window != 0) {
                    tg_gui_photo_viewer_paint(&viewer);
                }
            }
            pending = tg_gui_session_photo_pending() ||
                      tg_gui_photo_decode_pending(&viewer) ||
                      tg_gui_photo_cache_pending();
            events_pending = tg_gui_window_user_events_pending(&ctx, &viewer);
            reason = TG_GUI_PHOTO_STALL_NONE;
            if (pending && !done) {
                if (resize_pending || ctx.photo_resize_active) {
                    reason = TG_GUI_PHOTO_STALL_RESIZE;
                } else if (tg_gui_session_transfer_busy()) {
                    reason = TG_GUI_PHOTO_STALL_TRANSFER;
                } else if (photo_resume_turn) {
                    reason = TG_GUI_PHOTO_STALL_RESUME;
                } else if (events_pending) {
                    reason = TG_GUI_PHOTO_STALL_QUEUED_EVENT;
                } else if (interactive_event) {
                    reason = TG_GUI_PHOTO_STALL_INTERACTIVE;
                }
                if (reason == TG_GUI_PHOTO_STALL_NONE ||
                    ((reason == TG_GUI_PHOTO_STALL_INTERACTIVE ||
                      (TG_GUI_PHOTO_FORCE_QUEUED_EVENTS &&
                       reason == TG_GUI_PHOTO_STALL_QUEUED_EVENT)) &&
                     photo_defer_ticks >=
                         TG_GUI_PHOTO_MAX_DEFER_TICKS - 1)) {
                    if (photo_defer_ticks > 0 &&
                        (reason == TG_GUI_PHOTO_STALL_INTERACTIVE ||
                         reason == TG_GUI_PHOTO_STALL_QUEUED_EVENT) &&
                        photo_stall_reason != reason) {
                        tg_gui_photo_stall_diag(reason);
                    }
                    photo_background_turn = 1;
                    photo_defer_ticks = 0;
                    photo_stall_reason = TG_GUI_PHOTO_STALL_NONE;
                } else {
                    if (photo_defer_ticks < TG_GUI_PHOTO_MAX_DEFER_TICKS) {
                        ++photo_defer_ticks;
                    }
                    if (photo_defer_ticks >= TG_GUI_PHOTO_MAX_DEFER_TICKS &&
                        photo_stall_reason != reason) {
                        tg_gui_photo_stall_diag(reason);
                        photo_stall_reason = reason;
                    }
                }
            } else if (!pending) {
                photo_defer_ticks = 0;
                photo_stall_reason = TG_GUI_PHOTO_STALL_NONE;
            }
        }
        /* Explicit user transfers still win. Otherwise move one bounded cache
           chunk per permitted background turn. */
        if (photo_background_turn &&
            tg_gui_photo_cache_maintenance_tick(state, &viewer)) {
            session_dirty = 1;
        }
        if (photo_background_turn &&
            !tg_gui_photo_disk_cache.clear_pending &&
            tg_gui_session_photo_step(stdout)) {
            session_dirty = 1;
            viewer_dirty = 1;
        }
        if (tg_gui_photo_save_tick(state, &backend, &photo_save)) {
            session_dirty = 1;
        }
        /* Decode only on a permitted background turn. The budget begins at the
           old conservative slice and adapts from measured DateStamp time. A
           fast target may drain several local cache/decode stages inside the
           same ~120 ms envelope; queued input still stops the loop immediately. */
        if (photo_background_turn &&
            !tg_gui_photo_disk_cache.clear_pending) {
            unsigned long turn_start;
            int local_steps;

            turn_start = tg_gui_photo_now_ms();
            local_steps = 0;
            do {
                unsigned long slice_start;
                unsigned long slice_end;
                unsigned long slice_ms;
                unsigned long work_budget;
                unsigned long cache_budget;
                int viewer_used;
                int inline_used;
                int work_kind;

                work_budget = photo_decode_pace.budget;
                cache_budget = photo_cache_pace.budget;
                viewer_used = 0;
                inline_used = 0;
                work_kind = TG_GUI_PHOTO_WORK_NONE;
                slice_start = tg_gui_photo_now_ms();
                if (tg_gui_photo_viewer_decode_tick(
                        &viewer, (unsigned int)work_budget,
                        (int)photo_replay_pace.budget, cache_budget,
                        &viewer_used, &work_kind)) {
                    viewer_dirty = 1;
                }
                if (!viewer_used &&
                    tg_gui_photo_decode_tick(
                        &ctx, (unsigned int)work_budget,
                        (int)photo_replay_pace.budget, cache_budget,
                        &inline_used, &work_kind)) {
                    session_dirty = 1;
                }
                if (!viewer_used && !inline_used) {
                    break;
                }
                slice_end = tg_gui_photo_now_ms();
                slice_ms = tg_gui_photo_elapsed_ms(slice_start, slice_end);
                if (work_kind == TG_GUI_PHOTO_WORK_CACHE) {
                    tg_gui_photo_pace_observe_log(
                        "cache", &photo_cache_pace, slice_ms);
                    photo_fast_wake =
                        photo_cache_pace.budget > photo_cache_pace.minimum ||
                        slice_ms < photo_cache_pace.target_ms;
                } else if (work_kind == TG_GUI_PHOTO_WORK_DECODE) {
                    tg_gui_photo_pace_observe_log(
                        "decode", &photo_decode_pace, slice_ms);
                    photo_fast_wake =
                        photo_decode_pace.budget > photo_decode_pace.minimum ||
                        slice_ms < photo_decode_pace.target_ms;
                } else if (work_kind == TG_GUI_PHOTO_WORK_REPLAY) {
                    tg_gui_photo_pace_observe_log(
                        "replay", &photo_replay_pace, slice_ms);
                    photo_fast_wake =
                        photo_replay_pace.budget > photo_replay_pace.minimum ||
                        slice_ms < photo_replay_pace.target_ms;
                }
                ++local_steps;
                if (!tg_gui_photo_decode_pending(&viewer) ||
                    tg_gui_window_user_events_pending(&ctx, &viewer)) {
                    break;
                }
            } while (local_steps < TG_GUI_PHOTO_LOCAL_STEPS_MAX &&
                     tg_gui_photo_elapsed_ms(turn_start,
                                             tg_gui_photo_now_ms()) <
                         TG_GUI_PHOTO_PACE_TARGET_MS);
        }
        /* Load-older paging: a scroll-up reached the top of the transcript. "Top"
           INCLUDES the case where the whole backlog FITS the window (sb_tr_max==0,
           no scrollbar drawn): a wheel/cursor up there still means "load older",
           otherwise those chats could never page back (the reported bug -- the
           user had to shrink the window to make a scrollbar appear). The post-drain
           transcript_scroll is used so a drag/wheel that lands at the top fires in
           one gesture; the painter clamps it to sb_tr_max, so the >= test means
           "at the top". Synchronous fetch, like open_chat; a small cooldown keeps a
           fast wheel from hammering a slow link. */
        {
            int was_fits = (state->sb_tr_max == 0);
            int at_top = was_fits || (state->transcript_scroll >= state->sb_tr_max);
            if (want_older && !older_exhausted && older_cooldown == 0 && at_top &&
                tg_gui_session_is_open()) {
                /* Let the ring drop its newest tail ONLY when the newest is
                   off-screen (a scrollable transcript scrolled to the top); when
                   everything fits, the newest rows are visible -- never evict
                   them (would also lose their read-receipt state). */
                int got = tg_gui_session_load_older(stdout, state->sb_tr_max > 0);
                if (got > 0) {
                    scroll_dirty = 1;     /* new rows above -> repaint with them */
                    older_cooldown = 2;   /* breather before the next page */
                    if (was_fits) {
                        reveal_older = 1; /* fits-case: scroll to show the older */
                    }
                } else if (got == 0) {
                    older_exhausted = 1;  /* confirmed chat start / buffer full */
                    state->more_above = 0; /* nothing older -> drop the forced bar */
                }
                /* got < 0: transient fetch failure -> do NOT latch; retry later. */
            }
        }
        /* Heartbeat fired? Run the SAME poll the INTUITICKS path runs (same
           cadence guard via last_session_poll, same composing-idle deferral).
           The scheduler below re-arms it at the heartbeat or photo-work rate.
           This is what keeps reception alive while the window is
           inactive; when it is active the shared time guard prevents any
           double-polling. */
        if (timer_ok && timer_pending &&
            CheckIO((struct IORequest *)timer_req) != 0) {
            (void)WaitIO((struct IORequest *)timer_req);
            timer_pending = 0;
            if (!done && state->mode == TG_GUI_MODE_CHAT) {
                time_t hb_now = time(0);
                unsigned long hb_eff = watch_seconds;

                if (watch_boot_grace && session_boot != (time_t)-1 &&
                    hb_now != (time_t)-1 &&
                    (unsigned long)(hb_now - session_boot) < watch_boot_grace) {
                    hb_eff = watch_boot_seconds;
                }
                if (tg_gui_session_transfer_busy() &&
                    hb_eff < TG_GUI_TRANSFER_POLL_SECONDS) {
                    hb_eff = TG_GUI_TRANSFER_POLL_SECONDS;
                }
                if ((state->composing || tg_gui_session_transfer_busy()) &&
                    hb_now != (time_t)-1 &&
                    hb_now >= last_receive_drain &&
                    (unsigned long)(hb_now - last_receive_drain) >=
                        TG_GUI_COMPOSE_RECEIVE_SECONDS) {
                    last_receive_drain = hb_now;
                    if (tg_gui_session_receive_pending(stdout)) {
                        session_dirty = 1;
                    }
                }
                if (hb_now != (time_t)-1 &&
                    (unsigned long)(hb_now - last_session_poll) >= hb_eff &&
                    (!state->composing ||
                     (unsigned long)(hb_now - last_key_time) >=
                         TG_GUI_COMPOSE_IDLE_POLL_SECONDS)) {
                    last_session_poll = hb_now;
                    if (tg_gui_session_tick(stdout)) {
                        session_dirty = 1;
                    }
                }
            }
        }
        if (session_dirty || scroll_dirty) {
            tg_gui_window_paint(state, &backend);
        }
        if (viewer_dirty && viewer.ctx.window != 0) {
            tg_gui_photo_viewer_paint(&viewer);
        }
        /* A fits-window load left the older rows above the pinned-newest view: if
           it now overflows, scroll to the top to reveal them (the paint above
           refreshed sb_tr_max). If it still fits, they are already on screen. */
        if (reveal_older && state->sb_tr_max > 0) {
            state->transcript_scroll = state->sb_tr_max;
            tg_gui_window_paint(state, &backend);
        }
        /* Re-arm paging once the user scrolls back off the top of a SCROLLABLE
           transcript. When everything fits (sb_tr_max==0) there is no off-top to
           return to, so the latch persists until the chat is reopened -- correct,
           because with the tri-state return only a real chat-start sets it (a
           transient fetch failure returns < 0 and never latches). */
        if (state->sb_tr_max > 0 &&
            state->transcript_scroll < state->sb_tr_max) {
            older_exhausted = 0;
        }
        /* A (re)opened chat -- every open path moves selected_chat (F-keys, a row
           click, a search result) -- starts paging fresh, including the fits-case
           where the off-top re-arm above can never fire. */
        if (state->selected_chat != prev_selected) {
            older_exhausted = 0;
            older_cooldown = 0;
            prev_selected = state->selected_chat;
        }
        /* Fast measured slices keep a 150 ms wake while local photo work is
           pending. Once a slow machine settles at the conservative floor, or
           the queue empties, return to the original two-second heartbeat. */
        if (timer_ok) {
            int photo_pending_now;
            int want_fast_timer;

            photo_pending_now = tg_gui_session_photo_pending() ||
                                tg_gui_photo_decode_pending(&viewer) ||
                                tg_gui_photo_cache_pending();
            if (!photo_pending_now) {
                photo_fast_wake = 0;
            }
            want_fast_timer = tg_gui_photo_cache_pending() ||
                              (photo_pending_now && photo_fast_wake);
            if (timer_pending && timer_fast != want_fast_timer) {
                AbortIO((struct IORequest *)timer_req);
                (void)WaitIO((struct IORequest *)timer_req);
                timer_pending = 0;
            }
            if (!timer_pending) {
                tg_gui_timer_arm(timer_req, want_fast_timer);
                timer_pending = 1;
                timer_fast = want_fast_timer;
            }
        }
    }

    tg_platform_gui_drop_disarm(); /* before the window goes away */
    /* The viewer is a visitor on the same screen. Close it before releasing
       shared pens, the CyberGraphX interface or a private application screen. */
    tg_gui_photo_viewer_close(&viewer);
    /* Window going away with a transfer still running (menu Quit, iconify,
       Amiga+Q): cancel and unwind it -- end() closes the file (removing a
       partial download) so session_close finds the engine idle. */
    if (tg_gui_session_transfer_busy()) {
        tg_gui_session_transfer_cancel();
        (void)tg_gui_session_transfer_end(0, 0UL);
    }
    /* Detach + free the menu strip before the window goes away. */
    if (menu != 0) {
        ClearMenuStrip(ctx.window);
        FreeMenus(menu);
        menu = 0;
    }
    if (vi != 0) {
        FreeVisualInfo(vi);
        vi = 0;
    }
    /* Heartbeat teardown: abort any in-flight request BEFORE freeing (a
       firing request after DeleteIORequest is a guaranteed crash). */
    if (timer_ok) {
        if (timer_pending) {
            AbortIO((struct IORequest *)timer_req);
            (void)WaitIO((struct IORequest *)timer_req);
            timer_pending = 0;
        }
        CloseDevice((struct IORequest *)timer_req);
        timer_ok = 0;
    }
    if (timer_req != 0) {
        DeleteIORequest((struct IORequest *)timer_req);
        timer_req = 0;
    }
    if (timer_port != 0) {
        DeleteMsgPort(timer_port);
        timer_port = 0;
    }
    tg_gui_amiga_buffer_free(&ctx); /* free the off-screen double-buffer */
    tg_gui_log("window: releasing pens");
    tg_gui_amiga_release_pens(&ctx, cmap);
    tg_gui_amiga_close_cybergraphics();
    tg_gui_window_save_geom(ctx.inner_w, ctx.inner_h,
                            (int)ctx.window->LeftEdge,
                            (int)ctx.window->TopEdge, want_own);
    tg_gui_log("window: CloseWindow");
    CloseWindow(ctx.window);
    ctx.window = 0;
    if (own_scr != 0) {
        /* Private screen, our window was the only one -> CloseScreen succeeds
           deterministically; the bounded retry is purely defensive (leak
           rather than hang if something impossible keeps it open). */
        int scr_tries = 0;

        while (CloseScreen(own_scr) == FALSE && scr_tries < 10) {
            Delay(10);
            ++scr_tries;
        }
        if (scr_tries >= 10) {
            puts("gui window: own screen close blocked (leaked)");
        }
        own_scr = 0;
    }
    tg_gui_amiga_close_core_libs();
    tg_gui_log("window: libraries closed");
    /* 2 = iconified (park on AppIcon), 3 = reopen (own-screen toggle). */
    return (done == 2) ? 2 : (done == 3) ? 3 : 0;
}

/* Iconified park: an AppIcon on Workbench (our own shipped icon when
   loadable, the default tool icon otherwise); a double-click -- or a
   Ctrl-C break -- wakes us. Returns 1 to reopen the window, 0 to quit.
   Everything is opened lazily and torn down before returning, so the
   iconified footprint is just this task, its stack and the AppIcon. */
static int tg_gui_window_iconify_wait(void)
{
    struct MsgPort *port = 0;
    struct DiskObject *dobj = 0;
    struct AppIcon *icon = 0;
    struct Message *m;
    int reopen = 0;

    WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 36L);
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36L);
#if defined(__amigaos4__)
    if (WorkbenchBase != 0) {
        IWorkbench = (struct WorkbenchIFace *)GetInterface(WorkbenchBase,
                                                           "main", 1L, 0);
    }
    if (IconBase != 0) {
        IIcon = (struct IconIFace *)GetInterface(IconBase, "main", 1L, 0);
    }
    if (IWorkbench == 0 || IIcon == 0) {
        goto out;
    }
#endif
    if (WorkbenchBase == 0 || IconBase == 0) {
        goto out;
    }
    port = CreateMsgPort();
    if (port == 0) {
        goto out;
    }
    /* Our own program icon (TelegramAmiga.info) so the AppIcon shows the
       Telegram image, not a generic tool icon. The pre-0.0.6 name was
       "TelegramGUI"; after the rename that file no longer ships, so this used to
       miss and fall through to GetDefDiskObject (the generic icon seen on OS4). */
    dobj = GetDiskObject((STRPTR)"PROGDIR:TelegramAmiga");
    if (dobj == 0) {
        dobj = GetDefDiskObject(WBTOOL);
    }
    if (dobj == 0) {
        goto out;
    }
    dobj->do_CurrentX = NO_ICON_POSITION;
    dobj->do_CurrentY = NO_ICON_POSITION;
    icon = AddAppIcon(0UL, 0UL, (STRPTR)"TelegramAmiga", port, 0, dobj,
                      TAG_DONE);
    if (icon != 0) {
        ULONG sigs = Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);

        reopen = (sigs & (1UL << port->mp_SigBit)) != 0UL;
        while ((m = GetMsg(port)) != 0) {
            ReplyMsg(m);
        }
        RemoveAppIcon(icon);
        while ((m = GetMsg(port)) != 0) {
            ReplyMsg(m);
        }
    }
out:
    if (dobj != 0) {
        FreeDiskObject(dobj);
    }
    if (port != 0) {
        DeleteMsgPort(port);
    }
#if defined(__amigaos4__)
    if (IIcon != 0) {
        DropInterface((struct Interface *)IIcon);
        IIcon = 0;
    }
    if (IWorkbench != 0) {
        DropInterface((struct Interface *)IWorkbench);
        IWorkbench = 0;
    }
#endif
    if (IconBase != 0) {
        CloseLibrary(IconBase);
        IconBase = 0;
    }
    if (WorkbenchBase != 0) {
        CloseLibrary(WorkbenchBase);
        WorkbenchBase = 0;
    }
    return reopen;
}

int tg_gui_run_window(tg_gui_state *state)
{
    if (state == 0) {
        return 2;
    }
    if (state->photo_cache_limit_mb != TG_GUI_PHOTO_CACHE_UNLIMITED_MB &&
        state->photo_cache_limit_mb != 10UL &&
        state->photo_cache_limit_mb != 50UL &&
        state->photo_cache_limit_mb != 200UL) {
        state->photo_cache_limit_mb = TG_GUI_PHOTO_CACHE_DEFAULT_MB;
    }
    tg_gui_photo_cache_begin(state->photo_cache_limit_mb);
    for (;;) {
        int rc = tg_gui_run_window_once(state);

        if (rc == 3) {
            continue; /* own-screen toggle: reopen immediately, no AppIcon */
        }
        if (rc != 2) {
            tg_gui_photo_cache_end();
            return rc;
        }
        if (!tg_gui_window_iconify_wait()) {
            tg_gui_photo_cache_end();
            return 0; /* AppIcon failed or Ctrl-C: a clean quit */
        }
        /* Double-click: fall through and reopen the window fresh. */
    }
}

#else /* !TG_GUI_AMIGA: host build */

int tg_gui_run_window(tg_gui_state *state)
{
    (void)state;
    puts("gui window: native window not available on this build; "
         "use --gui-self-test for the layout check.");
    return 0;
}

void tg_gui_window_avatar_invalidate(unsigned long id_hi, unsigned long id_lo)
{
    (void)id_hi; /* no native window: nothing cached to drop */
    (void)id_lo;
}

void tg_gui_window_photo_cache_file_changed(const char *path)
{
    (void)path;
}

void tg_gui_window_photo_cache_file_removed(const char *path)
{
    (void)path;
}

#endif
