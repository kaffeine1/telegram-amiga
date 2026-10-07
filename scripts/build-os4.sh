#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Build the AmigaOS 4 binary CLEAN in the os4-cross docker workspace and COPY IT
# BACK into this repo's build/amigaos4/. OS4 is the only lane that builds in a
# separate workspace; forgetting the copy-back is what shipped a stale OS4 binary
# in the first 0.0.2 packaging. Run this before packaging a release.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OS4_CROSS=${OS4_CROSS:-"$HOME/amiga-dev/projects/os4-cross"}
WS="$OS4_CROSS/work/telegram-amiga"

[ -x "$OS4_CROSS/bin/os4-run" ] || { echo "ERROR: os4-cross not at $OS4_CROSS (set OS4_CROSS=...)" >&2; exit 1; }

# Only the files git tracks go across: the workspace is mounted into a
# third-party container, and an exclude list let private files through
# (a phone number, a login code, a token file, avatars, local notes).
echo "sync tracked files -> $WS"
mkdir -p "$WS"
git -C "$ROOT_DIR" ls-files -z | rsync -a --from0 --files-from=- "$ROOT_DIR/" "$WS/"

rm -f "$WS/build/amigaos4/TelegramAmiga"
cd "$OS4_CROSS"
# The container gcc throws sporadic cc1 ICEs under emulation; rerunning make
# WITHOUT clean resumes from the broken object. Try clean once, then resume.
attempt=1
while [ "$attempt" -le 6 ]; do
    [ "$attempt" = 1 ] && MK="clean all" || MK="all"
    ./bin/os4-run bash -lc "cd telegram-amiga && make -f Makefile.amigaos4 $MK TARGET=build/amigaos4/TelegramAmiga" >/dev/null 2>&1 || true
    [ -f "$WS/build/amigaos4/TelegramAmiga" ] && break
    echo "  attempt $attempt: cc1 ICE? resuming without clean..." >&2
    attempt=$((attempt + 1))
done

SRC="$WS/build/amigaos4/TelegramAmiga"
[ -f "$SRC" ] || { echo "ERROR: OS4 build failed after retries" >&2; exit 1; }

mkdir -p "$ROOT_DIR/build/amigaos4"
cp "$SRC" "$ROOT_DIR/build/amigaos4/TelegramAmiga"   # <-- the copy-back, the easy-to-forget step

if command -v md5 >/dev/null 2>&1; then
    md5sum_out=$(md5 -q "$ROOT_DIR/build/amigaos4/TelegramAmiga")
else
    md5sum_out=$(md5sum "$ROOT_DIR/build/amigaos4/TelegramAmiga" | awk '{print $1}')
fi
echo "OK -> build/amigaos4/TelegramAmiga  ($md5sum_out, $(wc -c < "$ROOT_DIR/build/amigaos4/TelegramAmiga" | tr -d ' ')b)"
