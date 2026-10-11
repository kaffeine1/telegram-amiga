#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Build the plain-68000 package's binary (A500 / A600 / A1000 / CDTV class):
# the text client only (NO_GUI), the low-memory caps and a 384 KB stack, into
# build/amigaos3-68000-lowmem/, where package-human-release.sh takes it from.
# The recipe used to live only in a note, and build/amigaos3-68000/ also held
# whatever 68000 build was made last (a GUI one, for a compile check), which
# packaging would have shipped without a word. Run this before packaging.
#
# EXTRA_CFLAGS adds to the recipe (a tester's -DTG_DIAG_TRACE=1 build); the
# packaging refuses such a binary, so a diagnostic build never ships.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="build/amigaos3-68000-lowmem"
TOOLCHAIN_BIN=${TOOLCHAIN_BIN:-"$HOME/amiga-dev/toolchain/bin"}

if [ -d "$TOOLCHAIN_BIN" ]; then
    PATH="$TOOLCHAIN_BIN:$PATH"
    export PATH
fi
command -v m68k-amigaos-gcc >/dev/null 2>&1 || {
    echo "ERROR: m68k-amigaos-gcc not found (set TOOLCHAIN_BIN=...)" >&2
    exit 1
}

# The caps keep the static data near 1 MB, so the whole program loads in
# about 1.5 MB on a 2 MB chip + 4 MB fast A600 with a PCMCIA network card.
LOWMEM_CFLAGS="-DTG_LOWMEM=1 -DTG_GUI_MAX_CHATS=12 -DTG_GUI_MSG_TEXT_MAX=768 \
-DTG_GUI_MAX_MESSAGES=12 -DTG_MTPROTO_MESSAGE_TEXT_LIST_MAX=8 \
-DTG_MTPROTO_MESSAGE_TEXT_MAX=2048 -DTG_MTPROTO_ENCRYPTED_BODY_MAX=24576 \
-DTG_GUI_DL_CHUNK=16384UL -DTG_GUI_DL_WBUF=32768UL \
-DTG_MTPROTO_REPLY_RECV_MAX=32768U -DTG_MTPROTO_GZIP_UNPACKED_MAX=32768UL"

cd "$ROOT_DIR"
make -f Makefile.amigaos3-clib2 clean all M68K_CPU=68000 NO_GUI=1 \
    TARGET="$OUT_DIR/TelegramAmiga" OBJDIR="$OUT_DIR/obj" \
    EXTRA_CFLAGS="$LOWMEM_CFLAGS ${EXTRA_CFLAGS:-}"

bin="$OUT_DIR/TelegramAmiga"
strings "$bin" | grep -F '$STACK:393216' >/dev/null || {
    echo "ERROR: $bin lacks the 384 KB stack cookie of the low-memory build" >&2
    exit 1
}
echo "OK -> $bin ($(wc -c < "$bin" | tr -d ' ') bytes)"
