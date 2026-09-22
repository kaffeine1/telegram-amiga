<!--
Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
SPDX-License-Identifier: MIT
-->

# Telegram Amiga — GUI architecture (Phase 5b)

Two product lines, one core. The user picks at launch (`--ui gui|console`,
or two launchers); neither is a compile-time platform gate.

- **Console / TUI line** — the current full-screen console chat
  (`tg_console.c`, `tg_console_ui.c`, `tg_console_tui.c`). Unchanged. For
  low-end hardware and as the universal fallback.
- **GUI line** — a native Intuition window (modern theme, proportional font,
  chat list on the left, message bubbles, initials avatars). New. Offered on
  every platform, fast 68k (040/060/080) included.

Both drive the same MTProto core and — once extracted — the same chat engine.

## Layers

1. **Core** (unchanged): MTProto, crypto, sessions, updates/pts, peer cache,
   file I/O. Portable, proven on six CPU/OS targets and on host CI.
2. **Chat engine** (to extract from `tg_mtproto_probe.c`): transcript model,
   chat list, notifications — event driven. Will drive both the console and
   the GUI. The TUI is the behavioural oracle during the extraction.
3a. **Console driver**: `tg_console*.c` (the TUI).
3b. **GUI driver**: `tg_gui.c` — portable model + renderer — over a thin
    per-platform `tg_gui_backend`.

## tg_gui_backend

The per-platform shim is deliberately thin. Portable code owns layout and
rendering; the backend provides only:

- metrics: `width`, `height`, `line_height`, `text_width`;
- drawing: `fill_rect`, `avatar_fill`, `draw_text` (the `pen` argument is a
  colour *role* the backend resolves for the active theme);
- (next increment) the window itself and an event pump built on a single
  `WaitSelect(socket_fds + IDCMP sigmask)` — one wait point, woken by network
  or by user input, no polling.

The host backend records the draw calls so `tg_gui_self_test()` can check the
layout maths on CI, exactly like the MTProto self-tests. The Amiga backends
draw the same calls to a RastPort.

## Rendering rule

Redraw only what changed, row by row (as the TUI already does). This is a
requirement, not an optimisation: the GUI must stay fluid on a 68k, since the
user may choose it there.

## Theme

Default is the modern dark theme. Light (following Workbench pens) and a
classic AmIRC skin are themes over the same pen roles — no renderer changes.

## Status

- [x] branch `gui-intuition`; portable model + renderer skeleton + host
  self-test (`--gui-self-test`)
- [x] milestone 0: Intuition backend + real window (`--gui-test`) — builds on
  all six targets, validated end to end on hosted AROS x86_64 (headless
  Xvfb), redraw-time + footprint measurement included; passed a 12-finding
  adversarial review. Still to run on the Vampire (68k gate, Michele's HW).
- [ ] chat engine extraction (TUI as oracle)
- [ ] reader → input/send → notifications → parity with the TUI
