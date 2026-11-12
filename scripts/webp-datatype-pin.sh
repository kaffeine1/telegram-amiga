#!/bin/sh
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The client offers to fetch its WEBP datatype from a Telegram channel and
# to install it, and keeps a file only when its SHA-256 is the one written
# in include/tg_version.h. This script ties the two ends together for one
# version of the datatype:
#
#   scripts/webp-datatype-pin.sh <builds> <version> [--write]
#
# <builds> is the build directory of the webp-datatype project, or any
# directory with os3/webp.datatype, os4/webp.datatype, morphos/webp.datatype
# and descriptor/WebP in it (the files of a release, taken out of its
# archives, laid out the same way). <version> is the datatype's, as in 45.1.
#
# It copies the four files into build/webp-datatype-post/ under the names
# the client looks for (those are the files to post in the channel, as
# files, one message each or together), prints the lines of
# include/tg_version.h that pin them, and with --write puts those lines in
# the header. Without --write it also says whether the header already
# agrees. A posted file is never replaced: a new build of the datatype is
# a new version, with new names.

set -eu

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
HEADER="$ROOT_DIR/include/tg_version.h"
OUT_DIR="$ROOT_DIR/build/webp-datatype-post"

if [ $# -lt 2 ]; then
    echo "usage: $0 <builds> <version> [--write]" >&2
    exit 2
fi
BUILDS=$1
VERSION=$2
WRITE=${3:-}

case "$VERSION" in
    *[!0-9.]*|"") echo "the version is digits and dots, as in 45.1" >&2; exit 2 ;;
esac

sha256of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}
sizeof() { wc -c < "$1" | tr -d ' '; }

# macro prefix, source file, posted name
LIST="OS3 os3/webp.datatype webp-$VERSION-os3.datatype
OS4 os4/webp.datatype webp-$VERSION-os4.datatype
MOS morphos/webp.datatype webp-$VERSION-morphos.datatype
DESC descriptor/WebP WebP-$VERSION.descriptor"

echo "$LIST" | while read -r macro src name; do
    if [ ! -f "$BUILDS/$src" ]; then
        echo "missing: $BUILDS/$src" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR"
LINES=$(echo "$LIST" | while read -r macro src name; do
    cp "$BUILDS/$src" "$OUT_DIR/$name"
    echo "#define TG_WEBP_DT_${macro}_NAME \"$name\""
    echo "#define TG_WEBP_DT_${macro}_SIZE $(sizeof "$OUT_DIR/$name")UL"
    echo "#define TG_WEBP_DT_${macro}_SHA256 \"$(sha256of "$OUT_DIR/$name")\""
done)

echo "The files to post in the channel are in $OUT_DIR:"
ls -l "$OUT_DIR" | awk 'NR > 1 {print "  " $5 "  " $NF}'
echo
echo "The lines of include/tg_version.h:"
echo "$LINES"
echo

differs=0
echo "$LINES" | while read -r line; do
    grep -qxF "$line" "$HEADER" || exit 1
done || differs=1

if [ "$WRITE" = "--write" ]; then
    TMP="$HEADER.pin.$$"
    cp "$HEADER" "$TMP"
    echo "$LINES" | while read -r define macro value; do
        if ! grep -q "^#define $macro " "$TMP"; then
            echo "the header has no line for $macro" >&2
            rm -f "$TMP"
            exit 1
        fi
        # the value holds no slash, ampersand or backslash: names, digits, hex
        sed "s/^#define $macro .*/#define $macro $value/" "$TMP" > "$TMP.2"
        mv "$TMP.2" "$TMP"
    done
    mv "$TMP" "$HEADER"
    echo "include/tg_version.h now pins version $VERSION."
elif [ "$differs" = 0 ]; then
    echo "include/tg_version.h already pins these files."
else
    echo "include/tg_version.h does NOT pin these files (run again with --write)."
fi
