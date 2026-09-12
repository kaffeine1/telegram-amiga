#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Post-publish check: download each GitHub release asset and confirm it matches
# the locally-built binary (md5), is the right architecture, leaks no session
# file, ships the flashless icon and the expected files. Catches a stale upload
# or a missed --clobber. VERSION defaults to include/tg_version.h and can be
# overridden: VERSION=0.0.7 sh scripts/verify-release.sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPO=${REPO:-kaffeine1/telegram-amiga}
VERSION=${VERSION:-$(sed -n 's/.*define TG_VERSION "\([^"]*\)".*/\1/p' \
    "$ROOT_DIR/include/tg_version.h" 2>/dev/null)}
VERSION=${VERSION:-0.0.0}
# Same rule as the packaging: three components means alpha, two means the beta
# numbering (0.1). The release tags follow it.
case $VERSION in
    *.*.*) CHANNEL=${CHANNEL:-alpha} ;;
    *)     CHANNEL=${CHANNEL:-beta} ;;
esac

md5of() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | awk '{print $1}'; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

verify() {
    tag=$1; zippat=$2; localbin=$3; archpat=$4
    if [ ! -f "$ROOT_DIR/$localbin" ]; then echo "SKIP $tag: no local build $localbin"; return; fi
    if ! gh release download "$tag" --repo "$REPO" --pattern "$zippat" -O "$TMP/$tag.zip" >/dev/null 2>&1; then
        echo "FAIL $tag: download failed"; fail=1; return
    fi
    unzip -p "$TMP/$tag.zip" "*/TelegramAmiga" > "$TMP/bin" 2>/dev/null
    want=$(md5of "$ROOT_DIR/$localbin"); got=$(md5of "$TMP/bin")
    arch=$(file "$TMP/bin" | grep -c "$archpat" || true)
    leak=$(unzip -l "$TMP/$tag.zip" | grep -icE "telegram-(auth|peers|seed|password|token)|phone-code-hash" || true)
    icon=$(unzip -p "$TMP/$tag.zip" "*/TelegramAmiga.info" 2>/dev/null | strings | grep -c "^TelegramAmiga$" || true)
    # Since 0.0.9 the TUI marker and its icon ship on the 68k lane only (the
    # other platforms reach the TUI from a Shell), so the expected file count
    # is one higher there. Everything else is identical on every lane.
    files=$(unzip -l "$TMP/$tag.zip" | grep -cE "Manual-EN.txt|Manuale-IT.txt|/TelegramAmiga$|/TelegramAmiga-TUI$|telegram-api.txt" || true)
    case $tag in
    os3-*) want_files=5 ;;
    *)     want_files=4 ;;
    esac
    ok=1
    [ "$want" = "$got" ] || { echo "FAIL $tag: published binary $got != local build $want"; ok=0; }
    [ "$arch" -ge 1 ]    || { echo "FAIL $tag: wrong architecture"; ok=0; }
    [ "$leak" = 0 ]      || { echo "FAIL $tag: SESSION FILE LEAK"; ok=0; }
    [ "$icon" -ge 1 ]    || { echo "FAIL $tag: TelegramAmiga.info not flashless"; ok=0; }
    [ "$files" = "$want_files" ] || { echo "FAIL $tag: $files/$want_files expected files"; ok=0; }
    if [ "$ok" = 1 ]; then echo "OK   $tag  [bin $(printf %.8s "$got")]"; else fail=1; fi
}

verify "os3-$CHANNEL-$VERSION"        "*amigaos3*"   build/amigaos3-clib2/TelegramAmiga  "AmigaOS"
verify "os4-$CHANNEL-$VERSION"        "*amigaos4*"   build/amigaos4/TelegramAmiga        "PowerPC"
verify "morphos-$CHANNEL-$VERSION"    "*morphos*"    build/morphos-cross/TelegramAmiga   "PowerPC"
verify "aros-i386-$CHANNEL-$VERSION"  "*aros-i386*"  build/aros-i386-abiv0/TelegramAmiga "80386"
verify "aros-x86_64-$CHANNEL-$VERSION" "*x86_64*"    build/aros-x86_64/TelegramAmiga     "x86-64"

if [ "$fail" = 0 ]; then
    echo "All published $VERSION assets match the local builds."
else
    echo "VERIFICATION FAILED -- re-upload the offending asset(s)." >&2
    exit 1
fi
