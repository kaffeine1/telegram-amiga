/*
 * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AROS x86_64 startup fix: a correct autoinit library-open/close loop.
 *
 * The AROS One x86_64 ABI (aros/x86_64/libcall.h) passes a library base to an
 * LVO callee in r12: AROS_LIBCALL_INIT saves the old r12 and loads the base
 * into it around the indirect call, AROS_LIBCALL_EXIT restores it afterwards.
 * The indirect call itself is not modelled as reading r12, so under register
 * pressure GCC 10.5.0 is free to schedule the r12 *restore* before the call --
 * which is exactly what the SDK's prebuilt libautoinit.a (_set_open_libraries_-
 * list) does: its loop spills the saved r12 to the stack and reloads it ahead
 * of OpenLibrary, leaving the library *version* in r12. At startup, before
 * main(), lddemon's OpenLibrary then reads the version as its SysBase and bus-
 * faults (Software Failure 0x80000002 in Lddemon_0_OpenLibrary). It happens to
 * survive on a dirtied machine when r12's stale value is a mapped address, so
 * it looks intermittent; on fresh memory it crashes reliably.
 *
 * The same library calls compiled in low-register-pressure functions keep the
 * saved r12 in a register and restore it *after* the call, so they are correct.
 * This file re-implements the autoinit loop with the OpenLibrary/CloseLibrary
 * calls isolated in noinline helpers (verified by disassembly to keep r12 = the
 * base across the call), and provides the LIBS set so the linker never pulls
 * the broken libraries.o from libautoinit.a. AROS-only; other targets and the
 * host build never see this translation unit.
 */

#if defined(__AROS__)

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <aros/symbolsets.h>
#include <aros/autoinit.h>

/* Defines the symbols the startup iterates (__LIBS_LIST__ / __RELLIBS_LIST__);
   the per-library entries are appended to these sets by -lcrt / -lstdlib (and
   ADD2LIBS users) via ADD2SET. Providing the full set of symbols that the SDK's
   libraries.o exports keeps the linker from pulling that broken object in. */
DEFINESET(LIBS);
DEFINESET(RELLIBS);

AROS_MAKE_ASM_SYM(int, dummy, __includelibrarieshandling, 0);
AROS_EXPORT_ASM_SYM(__includelibrarieshandling);
AROS_MAKE_ASM_SYM(int, dummyrel, __includerellibrarieshandling, 0);
AROS_EXPORT_ASM_SYM(__includerellibrarieshandling);

/* The whole point: OpenLibrary in a tiny function so r12 (= SysBase) stays live
   across the LVO call instead of being clobbered by a mis-scheduled restore. */
static struct Library *__attribute__((noinline))
tg_aros_open_one(struct ExecBase *SysBase, CONST_STRPTR name, ULONG version)
{
    return OpenLibrary(name, version);
}

static void __attribute__((noinline))
tg_aros_close_one(struct ExecBase *SysBase, struct Library *base)
{
    CloseLibrary(base);
}

int _set_open_libraries_list(const void *const list[], struct ExecBase *SysBase)
{
    int pos;
    struct libraryset *set;

    ForeachElementInSet(list, 1, pos, set)
    {
        LONG version = *set->versionptr;
        int do_not_fail = 0;

        if (version < 0) {
            /* A negative request version means "open if you can, but do not
               fail startup if absent" -- the real value is -(v+1). */
            version = -(version + 1);
            do_not_fail = 1;
        }

        *set->baseptr = tg_aros_open_one(SysBase, set->name, (ULONG)version);

        if (!do_not_fail && *set->baseptr == NULL) {
            return 0;
        }
    }

    return 1;
}

void _set_close_libraries_list(const void *const list[], struct ExecBase *SysBase)
{
    int pos;
    struct libraryset *set;

    ForeachElementInSet(list, 1, pos, set)
    {
        if (*set->baseptr) {
            tg_aros_close_one(SysBase, (struct Library *)*set->baseptr);
            *set->baseptr = NULL;
        }
    }
}

/* Relative-library variants (for resident bases that store their library
   pointers at an offset inside another base). Unused by this program, but
   provided so the SDK's libraries.o is fully replaced and never linked. */
int _set_open_rellibraries_list(APTR base, const void *const list[],
                                struct ExecBase *SysBase)
{
    int pos;
    struct rellibraryset *set;

    ForeachElementInSet(list, 1, pos, set)
    {
        LONG version = *set->versionptr;
        int do_not_fail = 0;
        void **baseptr = (void **)((char *)base + *set->baseoffsetptr);

        if (version < 0) {
            version = -(version + 1);
            do_not_fail = 1;
        }

        *baseptr = tg_aros_open_one(SysBase, set->name, (ULONG)version);

        if (!do_not_fail && *baseptr == NULL) {
            return 0;
        }
    }

    return 1;
}

void _set_close_rellibraries_list(APTR base, const void *const list[],
                                  struct ExecBase *SysBase)
{
    int pos;
    struct rellibraryset *set;

    ForeachElementInSet(list, 1, pos, set)
    {
        void **baseptr = (void **)((char *)base + *set->baseoffsetptr);

        if (*baseptr) {
            tg_aros_close_one(SysBase, (struct Library *)*baseptr);
            *baseptr = NULL;
        }
    }
}

#endif /* __AROS__ */
