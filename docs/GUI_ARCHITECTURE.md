<!--
Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
SPDX-License-Identifier: MIT
-->

# GUI architecture

One engine, two clients. The window client starts from the main icon
(`--gui-live`), the text client from the TUI icon or a Shell
(`--mtproto-start-file ...`). Both run on every platform.

## Layers

1. **Core**: MTProto, crypto, sessions, updates, peer cache, files
   (`tg_mtproto_*.c`, `tg_net.c`, `tg_file.c`). Portable C, the same on
   the six Amiga targets and on the host.
2. **Chat engine** (`tg_chat_engine.c`): the session model, the updates
   cursor and the notification queue, shared by both clients.
3. **Text client**: `tg_console_tui.c` (full-screen console),
   `tg_console_ui.c` (colours), `tg_console.c`.
4. **Window client**:
   - `tg_gui.c`: model and renderer, all of the layout;
   - `tg_gui_driver.c`: turns chat-engine rows into the GUI model;
   - `tg_gui_window.c`: the Intuition/GadTools backend (window, menus,
     events, drawing). One file for every Amiga system: AmigaOS 4 differs
     only in how libraries are opened;
   - `platforms/<target>/`: what stays system specific, such as the
     Workbench start, the AppIcon, sound and file drops.

Most of the protocol flow of both clients still lives in
`tg_mtproto_probe.c`.

## The backend

The renderer asks the backend (`tg_gui_backend` in `include/tg_gui.h`)
only for metrics and drawing: size, line height, text width, rectangles,
text, avatars, emoji glyphs and photos. Colours are pen roles that the
backend resolves for the theme. The host build records the calls instead
of drawing them, so `--gui-self-test` checks the layout on CI.

## Rules

- Redraw only what changed: the window has to stay usable on a 68k.
- Never put `tg_gui_state` on the stack; it is large. Check stack frames
  with `-fstack-usage`.

## Self-tests

`--gui-self-test`, `--chat-engine-self-test`, `--chat-render-self-test`,
`--chat-list-self-test` and `--gui-driver-self-test` run on the host in
CI. The Amiga builds leave the self-tests out (`NO_SELFTEST=1`).
