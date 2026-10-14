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
# The channel word comes from the header, the same one the About box and the
# banner print (TG_VERSION_CHANNEL), so the binary and the packaging cannot
# disagree; the dot count is only a fallback for a tree without that macro.
CHANNEL=${CHANNEL:-$(sed -n 's/.*define TG_VERSION_CHANNEL "\([^"]*\)".*/\1/p' "$(dirname "$0")/../include/tg_version.h" 2>/dev/null)}
if [ -z "$CHANNEL" ]; then
    case $VERSION in
        *.*.*) CHANNEL=alpha ;;
        *)     CHANNEL=beta ;;
    esac
fi

md5of() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | awk '{print $1}'; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
skipped=0

verify() {
    tag=$1; zippat=$2; localbin=$3; archpat=$4
    if [ ! -f "$ROOT_DIR/$localbin" ]; then echo "SKIP $tag: no local build $localbin"; skipped=1; return; fi
    if ! gh release download "$tag" --repo "$REPO" --pattern "$zippat" -O "$TMP/$tag.zip" >/dev/null 2>&1; then
        echo "FAIL $tag: download failed"; fail=1; return
    fi
    rm -f "$TMP/bin"
    if ! unzip -p "$TMP/$tag.zip" "*/TelegramAmiga" > "$TMP/bin" 2>/dev/null || [ ! -s "$TMP/bin" ]; then
        echo "FAIL $tag: missing inner TelegramAmiga binary"; fail=1; return
    fi
    want=$(md5of "$ROOT_DIR/$localbin"); got=$(md5of "$TMP/bin")
    arch=$(file "$TMP/bin" | grep -c "$archpat" || true)
    leak=$(unzip -l "$TMP/$tag.zip" | grep -icE "telegram-(auth|peers|seed|password|token)|phone-code-hash" || true)
    # The 68000 package ships the TUI icon alone (no window in that build).
    case $tag in
    os3-68000-*) iconfile="TelegramAmiga-TUI.info" ;;
    *)           iconfile="TelegramAmiga.info" ;;
    esac
    icon=$(unzip -p "$TMP/$tag.zip" "*/$iconfile" 2>/dev/null | strings | grep -c "^TelegramAmiga$" || true)
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
    [ "$icon" -ge 1 ]    || { echo "FAIL $tag: $iconfile not flashless"; ok=0; }
    [ "$files" = "$want_files" ] || { echo "FAIL $tag: $files/$want_files expected files"; ok=0; }
    if [ "$ok" = 1 ]; then echo "OK   $tag  [bin $(printf %.8s "$got")]"; else fail=1; fi
}

verify "os3-$CHANNEL-$VERSION"        "*amigaos3*"   build/amigaos3-clib2/TelegramAmiga  "AmigaOS"
verify "os4-$CHANNEL-$VERSION"        "*amigaos4*"   build/amigaos4/TelegramAmiga        "PowerPC"
verify "morphos-$CHANNEL-$VERSION"    "*morphos*"    build/morphos-cross/TelegramAmiga   "PowerPC"
verify "aros-i386-$CHANNEL-$VERSION"  "*aros-i386*"  build/aros-i386-abiv0/TelegramAmiga "80386"
verify "aros-x86_64-$CHANNEL-$VERSION" "*x86_64*"    build/aros-x86_64/TelegramAmiga     "x86-64"
verify "aros-aarch64-$CHANNEL-$VERSION" "*aarch64*"  build/aros-aarch64/TelegramAmiga    "aarch64"
verify "os3-68000-$CHANNEL-$VERSION"  "*amigaos3-68000*" build/amigaos3-68000-lowmem/TelegramAmiga "AmigaOS"

LHA_BIN=${LHA_BIN:-"$HOME/amiga-dev/tools/lha-src/src/lha"}
PACKAGE_ROOT=${PACKAGE_ROOT:-"$ROOT_DIR/build/human-releases"}
AMINET_ROOT=${AMINET_ROOT:-"$PACKAGE_ROOT/aminet"}
OS4DEPOT_ROOT=${OS4DEPOT_ROOT:-"$PACKAGE_ROOT/os4depot"}
AROSARCHIVES_ROOT=${AROSARCHIVES_ROOT:-"$PACKAGE_ROOT/arosarchives"}

verify_lha() {
    lhafile=$1; lhalabel=$2; localbin=$3; archpat=$4
    if [ ! -f "$lhafile" ]; then echo "SKIP $lhalabel: $lhafile not found"; skipped=1; return; fi
    if [ ! -f "$ROOT_DIR/$localbin" ]; then echo "SKIP $lhalabel: $localbin not found"; skipped=1; return; fi
    rm -rf "$TMP/lha" && mkdir -p "$TMP/lha"
    if ! ( cd "$TMP/lha" && "$LHA_BIN" xq "$lhafile" >/dev/null 2>&1 ); then
        echo "FAIL $lhalabel: extract failed"; fail=1; return
    fi
    if [ ! -f "$TMP/lha/TelegramAmiga/TelegramAmiga" ]; then
        echo "FAIL $lhalabel: missing inner TelegramAmiga/TelegramAmiga"; fail=1; return
    fi
    want=$(md5of "$ROOT_DIR/$localbin"); got=$(md5of "$TMP/lha/TelegramAmiga/TelegramAmiga")
    arch=$(file "$TMP/lha/TelegramAmiga/TelegramAmiga" | grep -c "$archpat" || true)
    leak=$("$LHA_BIN" l "$lhafile" | grep -icE "telegram-(auth|peers|seed|password|token)|phone-code-hash" || true)
    ok=1
    [ "$want" = "$got" ] || { echo "FAIL $lhalabel: packaged binary $got != local build $want"; ok=0; }
    [ "$arch" -ge 1 ]    || { echo "FAIL $lhalabel: wrong architecture"; ok=0; }
    [ "$leak" = 0 ]      || { echo "FAIL $lhalabel: SESSION FILE LEAK"; ok=0; }
    if [ "$ok" = 1 ]; then echo "OK   $lhalabel  [bin $(printf %.8s "$got")]"; else fail=1; fi
}

if [ ! -x "$LHA_BIN" ]; then
    echo "SKIP LhA archives: no executable LHA_BIN ($LHA_BIN)"
    skipped=1
else
    verify_lha "$AMINET_ROOT/TelegramAmiga.lha"        "aminet/TelegramAmiga.lha"        build/amigaos3-clib2/TelegramAmiga        "AmigaOS"
    verify_lha "$AMINET_ROOT/TelegramAmiga-OS4.lha"    "aminet/TelegramAmiga-OS4.lha"    build/amigaos4/TelegramAmiga              "PowerPC"
    verify_lha "$AMINET_ROOT/TelegramAmiga-MOS.lha"    "aminet/TelegramAmiga-MOS.lha"    build/morphos-cross/TelegramAmiga         "PowerPC"
    verify_lha "$AMINET_ROOT/TelegramAmiga-AROS.lha"   "aminet/TelegramAmiga-AROS.lha"   build/aros-i386-abiv0/TelegramAmiga       "80386"
    verify_lha "$AMINET_ROOT/TelegramAmiga-AROS64.lha" "aminet/TelegramAmiga-AROS64.lha" build/aros-x86_64/TelegramAmiga           "x86-64"
    verify_lha "$AMINET_ROOT/TelegramAmiga-ARM64.lha"  "aminet/TelegramAmiga-ARM64.lha"  build/aros-aarch64/TelegramAmiga          "aarch64"
    verify_lha "$AMINET_ROOT/TelegramAmiga-68000.lha"  "aminet/TelegramAmiga-68000.lha"  build/amigaos3-68000-lowmem/TelegramAmiga "AmigaOS"
    verify_lha "$OS4DEPOT_ROOT/telegramamiga.lha"      "os4depot/telegramamiga.lha"      build/amigaos4/TelegramAmiga              "PowerPC"
    verify_lha "$AROSARCHIVES_ROOT/telegramamiga.i386-aros.lha"       "arosarchives/telegramamiga.i386-aros.lha"       build/aros-i386-abiv0/TelegramAmiga "80386"
    verify_lha "$AROSARCHIVES_ROOT/telegramamiga.x86_64-aros-v11.lha" "arosarchives/telegramamiga.x86_64-aros-v11.lha" build/aros-x86_64/TelegramAmiga     "x86-64"
    verify_lha "$AROSARCHIVES_ROOT/telegramamiga.aarch64-aros.lha"    "arosarchives/telegramamiga.aarch64-aros.lha"    build/aros-aarch64/TelegramAmiga    "aarch64"
fi

if [ "$fail" = 0 ]; then
    if [ "$skipped" = 1 ]; then
        echo "Checked $VERSION assets match the local builds (some checks were skipped)."
    else
        echo "All published $VERSION assets match the local builds."
    fi
else
    echo "VERIFICATION FAILED -- re-upload the offending asset(s)." >&2
    exit 1
fi
