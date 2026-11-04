<!--
Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
SPDX-License-Identifier: MIT
-->

# Contributing

Contributions are welcome when they are small, build, and come with a
test.

## Principles

- Portable code goes in `core/`, system differences in
  `platforms/<target>/`.
- C89, and no dependencies that are hard to get on an Amiga.
- Small steps that build, not large rewrites.
- Say what you tested: platform, real or emulated, toolchain, command.

## LLM agents

Patches prepared with LLM tools are accepted when a person has reviewed
them and they come with real or reproducible tests. Say whether the change
ran on real hardware, in an emulator, or was only compiled.

## Targets

| Makefile | Target |
|---|---|
| `Makefile.amigaos3-clib2` | AmigaOS 3.x, m68k (the 68000 build: `scripts/build-68000-lowmem.sh`) |
| `Makefile.amigaos4` | AmigaOS 4, PPC (Docker cross build: `scripts/build-os4.sh`) |
| `Makefile.morphos-cross` | MorphOS, PPC (see [docs/MORPHOS_CROSS_BUILD.md](docs/MORPHOS_CROSS_BUILD.md)) |
| `Makefile.aros-i386-abiv0` | AROS i386, ABIv0 |
| `Makefile.aros-x86_64` | AROS x86_64, ABIv11 |
| `Makefile.aros-aarch64` | AROS aarch64, ABIv1 (Raspberry Pi) |
| `Makefile.host` | Linux or macOS, for the self-tests |

You do not need every system, but a change must not break the others on
purpose.

## Before a pull request

- Build the target you changed, and the host build with warnings as
  errors:

  ```sh
  make -f Makefile.host clean all CFLAGS="-Wall -Wextra -Werror -O2"
  ```

- Run the self-tests CI runs: `--mtproto-self-test`, `--gui-self-test`,
  `--chat-engine-self-test`, `--chat-render-self-test`,
  `--chat-list-self-test` and `--gui-driver-self-test`.
- Keep the author and licence headers.
- Note user-visible changes in `CHANGELOG.md` under `[Unreleased]`.

## License

Contributions are distributed under the repository's MIT License.
