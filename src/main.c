/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "tg_app.h"
#include "tg_gui_session.h" /* tg_gui_log: diagnostic trace */
#include "tg_version.h"

/* Amiga $VER version tag (GitHub issue #3): lets the shell "Version" command
   report the binary's version on every Amiga-like OS. Must be a real string
   in the data section -- "used" keeps it alive if the linker ever GCs. */
#if defined(__GNUC__)
__attribute__((used))
#endif
static const char tg_amiga_ver_tag[] =
    "$VER: TelegramAmiga " TG_VERSION " (" TG_VERSION_DATE ")";
#include "tg_platform.h"

/* AmigaDOS scans this application-level cookie before entering main(). Keep it
   in the shared entry module so OS3, OS4 and both AROS binaries cannot drift.
   It is inert in the host test binary, where CI can still prove it survived
   linking. MorphOS additionally needs its PPC-specific __stack declaration. */
#if defined(__GNUC__)
__attribute__((used))
#endif
static const char tg_amiga_stack_cookie[] = TG_PLATFORM_SAFE_STACK_COOKIE;


/* This 68k build uses 68020 instructions (the 32x32 multiply among them), so
   on a plain 68000 the very first one is an illegal instruction and the user
   gets a bare "#80000004" alert with nothing to act on. Every package ships a
   binary named TelegramAmiga, so picking the wrong archive is easy: say so in
   words instead, and point at the package that does run there. */
#if defined(__amigaos3__) && !defined(TG_LOWMEM)
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <intuition/intuition.h>

static int tg_main_cpu_is_supported(int workbench)
{
    struct Library *ib;
    struct EasyStruct es;

    if ((SysBase->AttnFlags & AFF_68020) != 0) {
        return 1;
    }
    if (!workbench) {
        puts("This build needs a 68020 or better CPU.");
        puts("Use the AmigaOS 3.x (68000) package on this machine.");
        return 0;
    }
    ib = OpenLibrary((CONST_STRPTR)"intuition.library", 36L);
    if (ib != 0) {
        struct IntuitionBase *saved = IntuitionBase;

        IntuitionBase = (struct IntuitionBase *)ib;
        es.es_StructSize = (ULONG)sizeof(es);
        es.es_Flags = 0UL;
        es.es_Title = (STRPTR)TG_APP_TITLE;
        es.es_TextFormat =
            (STRPTR)"This build needs a 68020 or better CPU.\n\n"
                    "On a plain 68000 use the AmigaOS 3.x (68000)\n"
                    "package instead: it is built for this machine.";
        es.es_GadgetFormat = (STRPTR)"OK";
        (void)EasyRequestArgs(0, &es, 0, 0);
        IntuitionBase = saved;
        CloseLibrary(ib);
    }
    return 0;
}
#endif

/* One line describing the machine this build actually landed on: CPU flags,
   the stack it really got and the largest free memory block. A field report
   then answers by itself the two questions a bare Guru cannot: is this the
   right binary for this CPU, and was memory tight when it died. */
#if defined(TG_DIAG_TRACE) && defined(__amigaos3__)
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>

static const char *tg_diag_machine_line(void)
{
    static char line[128];
    struct Task *task;
    unsigned long stack;

    task = FindTask(0);
    stack = 0UL;
    if (task != 0 && task->tc_SPUpper != 0 && task->tc_SPLower != 0) {
        stack = (unsigned long)((unsigned char *)task->tc_SPUpper -
                                (unsigned char *)task->tc_SPLower);
    }
    sprintf(line,
            "diag: attnflags 0x%04lx stack %lu fast-largest %lu chip-largest %lu",
            (unsigned long)SysBase->AttnFlags, stack,
            (unsigned long)AvailMem(MEMF_FAST | MEMF_LARGEST),
            (unsigned long)AvailMem(MEMF_CHIP | MEMF_LARGEST));
    return line;
}
#endif

/* Scan the WBStartup arg names for "TUI" (case-insensitive). Amiga-only; the
   host build has no Workbench and returns 0. Defensive: any null -> GUI. */
#if defined(__amigaos3__) || defined(__amigaos4__) || defined(__MORPHOS__) || \
    defined(__AROS__)
#include <workbench/startup.h>
static int tg_main_wb_wants_tui(char **argv)
{
    struct WBStartup *wb = (struct WBStartup *)argv;
    long i;

    if (wb == 0 || wb->sm_ArgList == 0) {
        return 0;
    }
    for (i = 0; i < wb->sm_NumArgs; ++i) {
        const char *n = (const char *)wb->sm_ArgList[i].wa_Name;
        const char *p;

        for (p = n; p != 0 && *p != '\0'; ++p) {
            if ((p[0] == 'T' || p[0] == 't') &&
                (p[1] == 'U' || p[1] == 'u') &&
                (p[2] == 'I' || p[2] == 'i')) {
                return 1;
            }
        }
    }
    return 0;
}
#else
static int tg_main_wb_wants_tui(char **argv)
{
    (void)argv;
    return 0;
}
#endif

static int tg_main_finish(int result)
{
#ifdef TG_DIAG_TRACE
    tg_gui_log("diag: platform shutdown");
#endif
    tg_platform_shutdown();
#ifdef TG_DIAG_TRACE
    /* Last probe we control: whatever happens after this line happens in
       the C runtime's own exit path (stdio close, library close, WBStartup
       reply, UnLoadSeg). */
    tg_gui_log("diag: shutdown done, returning to runtime exit");
#endif
    return result;
}

static int tg_main_body(int argc, char **argv)
{
#if defined(TG_DIAG_TRACE)
    /* Diagnostic build: arm the crash-safe trail for EVERY launch, not just
       the Workbench one. A machine booted without its startup-sequence has no
       Workbench at all, and that is exactly the run whose log we want. */
    tg_gui_log_enable();
#if defined(__amigaos3__)
    if (argc != 0) {
        tg_gui_log("diag: main, shell launch");
        tg_gui_log(tg_diag_machine_line());
    }
#endif
#endif
    /*
     * Workbench launch (icon double-click): the Amiga C runtimes (clib2 on
     * m68k/OS4, libnix on MorphOS, AROS) call main() with argc == 0 and the
     * WBStartup message already stored and auto-replied at exit. There is no
     * console, so behave like the GUI launcher script did: send stdout/stderr
     * to NIL: (otherwise the runtime opens a console window for the binary's
     * log lines -- exactly what the IconX launcher avoided with "Run >NIL:"),
     * CurrentDir to PROGDIR:, and run the live GUI against the peer cache next
     * to the binary. CLI use is untouched (argc >= 1 always parses normally),
     * so this is fail-safe: if a runtime ever does not zero argc, the feature
     * simply does not activate.
     */
#if defined(__amigaos3__) && !defined(TG_LOWMEM)
    if (!tg_main_cpu_is_supported(argc == 0)) {
        return 20;
    }
#endif
    if (argc == 0) {
        /* Workbench: pick GUI vs TUI from the launch icon's name. A tool icon
           for the binary launches the GUI; the TUI ships as a project icon
           named "...-TUI" whose DefaultTool is this binary, so scanning the
           WBStartup arg names for "TUI" tells the two apart -- no wrapper
           scripts (papiosaur / Easy2Install suggestion). */
        /* Prefer an explicit TUI_MODE tooltype on the launched icon (issue #9,
           javierdlr); fall back to the filename heuristic so the default
           byte-identical icons still work with no tooltype at all. */
        int tt;
        int want_tui;

#ifdef TG_DIAG_TRACE
        /* Diagnostic build (experimental 68000 lane): the trail starts at the
           very first instruction of main, so a field report can tell "never
           reached main" from "died at step N". */
        tg_gui_log_enable();
        tg_gui_log("diag: main, workbench launch");
#if defined(__amigaos3__)
        tg_gui_log(tg_diag_machine_line());
#endif
#endif
        tt = tg_platform_wb_tui_mode(argv);
        want_tui = (tt >= 0) ? tt : tg_main_wb_wants_tui(argv);
#ifdef TG_DIAG_TRACE
        tg_gui_log(want_tui ? "diag: launching TUI" : "diag: launching GUI");
#endif

        if (want_tui) {
            static char *tui_argv[6];

#ifdef TG_DIAG_TRACE
            tg_gui_log("diag: workbench_init");
#endif
            tg_platform_workbench_init();
#ifdef TG_DIAG_TRACE
            tg_gui_log("diag: opening TUI console");
#endif
            if (!tg_platform_workbench_tui_console()) {
#ifdef TG_DIAG_TRACE
                tg_gui_log("diag: NO CONSOLE, giving up");
#endif
                return tg_main_finish(0); /* no console possible */
            }
#ifdef TG_DIAG_TRACE
            tg_gui_log("diag: console ready, stdio rebound");
#endif
            /* One line of drop status in the console scrollback: when a field
               report says "drag-and-drop does nothing", this says WHY. */
            printf("[file drag-and-drop: %s]\n",
                   tg_platform_console_drop_diag());
            tui_argv[0] = "TelegramAmiga";
            tui_argv[1] = "--mtproto-start-file";
            tui_argv[2] = "data/telegram-api.txt";
            tui_argv[3] = "telegram-auth.bin";
            tui_argv[4] = "data/phone-code-hash.txt";
            tui_argv[5] = "data/telegram-peers.txt";
            {
                int rc;

#ifdef TG_DIAG_TRACE
                tg_gui_log("diag: entering tg_app_run (TUI)");
#endif
                rc = tg_app_run(6, tui_argv);
#ifdef TG_DIAG_TRACE
                tg_gui_log("diag: tg_app_run returned");
#endif

                /* Farewell hint: the console teardown holds the window open
                   for one last look and waits for a single keypress (a
                   close-click EOF also counts where the handler sends one),
                   then the window closes for real. */
                /* The pause reads the cooked (line-buffered) console, so
                   only RETURN ends it -- say exactly that. */
                printf("\n--- Telegram Amiga closed. "
                       "Press RETURN to dismiss this window. ---\n");
                fflush(stdout);
#ifdef TG_DIAG_TRACE
                tg_gui_log("diag: farewell printed");
#endif
                /* Give the CON: handle back, or the window can never die:
                   the close gadget only works once every handle is gone. */
                tg_platform_workbench_tui_console_close();
#ifdef TG_DIAG_TRACE
                tg_gui_log("diag: console closed");
#endif
                return tg_main_finish(rc);
            }
        } else {
            static char *wb_argv[3];
            FILE *redir;
            /* GUI: no console -- redirect BEFORE any output so the lazy console
               window is never opened. */
            redir = freopen("NIL:", "w", stdout);
            (void)redir;
            redir = freopen("NIL:", "w", stderr);
            (void)redir;
            tg_platform_workbench_init();
            wb_argv[0] = "TelegramAmiga";
            wb_argv[1] = "--gui-live";
            wb_argv[2] = "data/telegram-peers.txt";
            return tg_main_finish(tg_app_run(3, wb_argv));
        }
    }
    return tg_main_finish(tg_app_run(argc, argv));
}

int main(int argc, char **argv)
{
    return tg_platform_run_with_safe_stack(tg_main_body, argc, argv);
}
