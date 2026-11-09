#!/bin/sh
#
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the Docker image containing pkgsrc cross/ppc-morphos-gcc.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${IMAGE:-telegram-amiga-morphos-cross:pkgsrc-2026Q1}
PKGSRC_BRANCH=${PKGSRC_BRANCH:-pkgsrc-2026Q1}
MAKE_JOBS=${MAKE_JOBS:-4}
ACCEPT_MORPHOS_SDK_LICENSE=${ACCEPT_MORPHOS_SDK_LICENSE:-0}
ACCEPT_LHA_LICENSE=${ACCEPT_LHA_LICENSE:-0}
MORPHOS_CROSS_DOCKERFILE=${MORPHOS_CROSS_DOCKERFILE:-}

DOCKERFILE_TMP=
cleanup_tmp() {
    if [ -n "$DOCKERFILE_TMP" ] && [ -f "$DOCKERFILE_TMP" ]; then
        rm -f "$DOCKERFILE_TMP"
    fi
}
trap cleanup_tmp EXIT

if [ -n "$MORPHOS_CROSS_DOCKERFILE" ]; then
    if [ ! -f "$MORPHOS_CROSS_DOCKERFILE" ]; then
        echo "MorphOS cross Dockerfile not found: $MORPHOS_CROSS_DOCKERFILE" >&2
        exit 1
    fi
    DOCKERFILE_PATH="$MORPHOS_CROSS_DOCKERFILE"
else
    DOCKERFILE_PATH=$(mktemp -t telegram-amiga-morphos-cross-XXXXXX.Dockerfile)
    DOCKERFILE_TMP=$DOCKERFILE_PATH
    cat > "$DOCKERFILE_PATH" <<'EOF'
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: GPL-3.0-or-later

FROM debian:bookworm

ARG PKGSRC_BRANCH=pkgsrc-2026Q1
ARG PKGSRC_PREFIX=/opt/pkg
ARG MAKE_JOBS=4
ARG ACCEPT_MORPHOS_SDK_LICENSE=0
ARG ACCEPT_LHA_LICENSE=0

ENV DEBIAN_FRONTEND=noninteractive
ENV PATH="${PKGSRC_PREFIX}/gg/bin:${PKGSRC_PREFIX}/bin:${PKGSRC_PREFIX}/sbin:${PATH}"

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        bison \
        build-essential \
        ca-certificates \
        curl \
        file \
        flex \
        gawk \
        git \
        gzip \
        m4 \
        make \
        patch \
        perl \
        rsync \
        sed \
        tar \
        xz-utils && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr

RUN curl -fsSL "https://cdn.NetBSD.org/pub/pkgsrc/${PKGSRC_BRANCH}/pkgsrc.tar.gz" | \
    tar -xz && \
    cd /usr/pkgsrc/bootstrap && \
    ./bootstrap --prefix "${PKGSRC_PREFIX}" --pkgdbdir "${PKGSRC_PREFIX}/pkgdb"

RUN if [ "${ACCEPT_MORPHOS_SDK_LICENSE}" != "1" ]; then \
        echo "MorphOS SDK license not accepted."; \
        echo "Rebuild with ACCEPT_MORPHOS_SDK_LICENSE=1 after reviewing the MorphOS SDK license."; \
        echo "pkgsrc package: /usr/pkgsrc/cross/ppc-morphos-sdk"; \
        exit 1; \
    fi && \
    if [ "${ACCEPT_LHA_LICENSE}" != "1" ]; then \
        echo "lha license not accepted."; \
        echo "Rebuild with ACCEPT_LHA_LICENSE=1 after reviewing the lha license."; \
        echo "pkgsrc package: /usr/pkgsrc/archivers/lha"; \
        exit 1; \
    fi && \
    echo "ACCEPTABLE_LICENSES+= morphos-sdk-license" >> "${PKGSRC_PREFIX}/etc/mk.conf" && \
    echo "ACCEPTABLE_LICENSES+= lha-license" >> "${PKGSRC_PREFIX}/etc/mk.conf" && \
    cd /usr/pkgsrc/cross/ppc-morphos-gcc && \
    bmake MAKE_JOBS="${MAKE_JOBS}" install clean clean-depends

WORKDIR /work

CMD ["sh", "-lc", "make -f Makefile.morphos-cross clean all"]
EOF
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found. Install Docker or start Colima/Docker Desktop first." >&2
    exit 1
fi

if [ "$ACCEPT_MORPHOS_SDK_LICENSE" != "1" ]; then
    echo "MorphOS SDK license not accepted." >&2
    echo "Review the MorphOS SDK license, then rerun with:" >&2
    echo "  ACCEPT_MORPHOS_SDK_LICENSE=1 $0" >&2
    exit 1
fi

if [ "$ACCEPT_LHA_LICENSE" != "1" ]; then
    echo "lha license not accepted." >&2
    echo "Review the lha license, then rerun with:" >&2
    echo "  ACCEPT_LHA_LICENSE=1 $0" >&2
    exit 1
fi

docker build \
    --build-arg "PKGSRC_BRANCH=$PKGSRC_BRANCH" \
    --build-arg "MAKE_JOBS=$MAKE_JOBS" \
    --build-arg "ACCEPT_MORPHOS_SDK_LICENSE=$ACCEPT_MORPHOS_SDK_LICENSE" \
    --build-arg "ACCEPT_LHA_LICENSE=$ACCEPT_LHA_LICENSE" \
    -t "$IMAGE" \
    -f "$DOCKERFILE_PATH" \
    "$ROOT_DIR"
