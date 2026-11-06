<!--
Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
SPDX-License-Identifier: MIT
-->

# MorphOS cross build

The MorphOS releases are built in Docker with the pkgsrc cross toolchain
(`cross/ppc-morphos-gcc`, `cross/ppc-morphos-binutils`,
`cross/ppc-morphos-sdk`, branch `pkgsrc-2026Q1`).

## The toolchain image

Start Docker Desktop or Colima, then:

```sh
ACCEPT_MORPHOS_SDK_LICENSE=1 ACCEPT_LHA_LICENSE=1 \
scripts/docker-morphos-cross-image.sh
```

The first run bootstraps pkgsrc under `/opt/pkg` and builds the compiler,
which takes a while. The two variables accept the licences pkgsrc asks
for: the MorphOS SDK's and that of `archivers/lha`, which unpacks it.

Overrides: `MAKE_JOBS=8`, `PKGSRC_BRANCH=...`, `IMAGE=...`, and
`MORPHOS_CROSS_DOCKERFILE=/path/to/Dockerfile` to use your own file
instead of the embedded template.

## The build

```sh
scripts/docker-morphos-cross-build.sh
```

It runs `Makefile.morphos-cross` and writes
`build/morphos-cross/Amigram`. TLS is off by default (the client
does not need it; see [TLS_CERTIFICATES.md](TLS_CERTIFICATES.md)).

After a build, check the binary's time stamp: when Docker is not running
the script prints an error without the word "error", and the old binary
stays in place.

## On MorphOS

Copy the binary into the drawer on a hard disk, such as `Work:TGh`, not
RAM: (running from RAM: crashed), with wget from an HTTP server on the
build machine, then `Protect Amigram +e`. The release builds
leave the self-tests out (`NO_SELFTEST=1`); run those on the host.
