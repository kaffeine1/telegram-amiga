#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Build the MorphOS tester with the pkgsrc ppc-morphos cross toolchain image.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${IMAGE:-telegram-amiga-morphos-cross:pkgsrc-2026Q1}
TARGET=${TARGET:-build/morphos-cross/Amigram}
ENABLE_TLS=${ENABLE_TLS:-0}
OPENSSL_CFLAGS=${OPENSSL_CFLAGS:-}
OPENSSL_LDFLAGS=${OPENSSL_LDFLAGS:-}

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found. Install Docker or start Colima/Docker Desktop first." >&2
    exit 1
fi

docker run --rm \
    -e "ENABLE_TLS=$ENABLE_TLS" \
    -e "TARGET=$TARGET" \
    -e "OPENSSL_CFLAGS=$OPENSSL_CFLAGS" \
    -e "OPENSSL_LDFLAGS=$OPENSSL_LDFLAGS" \
    -v "$ROOT_DIR:/work" \
    -w /work \
    "$IMAGE" \
    sh -lc '
        CC=${CC:-ppc-morphos-gcc}
        if ! command -v "$CC" >/dev/null 2>&1; then
            CC=$(find /opt/pkg -name ppc-morphos-gcc -type f | head -1)
        fi
        test -n "$CC"
        make -f Makefile.morphos-cross clean all \
            CC="$CC" \
            ENABLE_TLS="$ENABLE_TLS" \
            TARGET="$TARGET" \
            OPENSSL_CFLAGS="$OPENSSL_CFLAGS" \
            OPENSSL_LDFLAGS="$OPENSSL_LDFLAGS"
    '
