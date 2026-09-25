#!/bin/sh
# Regenerates the launcher icons of every lane from the artwork in
# assets/icons/carlo-spadoni/. The result is committed, so this only runs
# when the artwork or the icon scripts change. The OS3 step needs Pillow:
# PYTHON=/path/to/python-with-pillow scripts/build-icons.sh
#
# Every lane gets TelegramAmiga.info (the program, a PROJECT icon whose
# DefaultTool is the binary and whose stack is 1 MB), TelegramAmiga-TUI.info
# (the SAME bytes: the file name alone picks the console client) and
# drawer.info, which the packaging places next to the drawer under the
# drawer's own name.
set -e
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}
SRC=assets/icons/carlo-spadoni
OUT=assets/icons

mkdir -p "$OUT/amigaos4" "$OUT/amigaos3" "$OUT/amigaos3-68000" "$OUT/morphos" "$OUT/aros"

# AmigaOS 4: classic + ARGB, as drawn; only the launcher fields change.
"$PY" scripts/make_gui_icon.py "$SRC/OS4/OS4-1.info" "$OUT/amigaos4/TelegramAmiga.info" --project
"$PY" scripts/make_gui_icon.py "$SRC/OS4/OS4-2.info" "$OUT/amigaos4/drawer.info" --drawer

# AmigaOS 3.x: an OS3.5 colour icon (256 colours, rim matted over the
# Workbench grey, the drawing with its alpha in ARGB chunks too) with a planar
# fallback for 3.1. Both come from Carlo's 46 pixel OS3 drawings: the 64
# pixel AmigaOS 4 one was tried for the program icon and was too big on a
# Vampire's Workbench; 256 colours and the blended rim keep the small one
# clean. The
# 68000 build asks for 384 KB of stack, not 1 MB: a 2 MB machine cannot
# spare the megabyte and the binary's cookie says so.
"$PY" scripts/make_os35_icon.py "$SRC/OS3/OS3-1.info" "$OUT/amigaos3/TelegramAmiga.info" --project --frameless --colors 256
# The drawer keeps the cabinet Carlo drew for 3.x (46 pixels, his call).
"$PY" scripts/make_os35_icon.py "$SRC/OS3/OS3-2.info" "$OUT/amigaos3/drawer.info" --drawer --frameless --colors 256
"$PY" scripts/make_os35_icon.py "$SRC/OS3/OS3-1.info" "$OUT/amigaos3-68000/TelegramAmiga.info" --project --frameless --colors 256 --stack 393216
"$PY" scripts/make_os35_icon.py "$SRC/OS3/OS3-2.info" "$OUT/amigaos3-68000/drawer.info" --drawer --frameless --colors 256 --stack 393216

# MorphOS and AROS: PNG icons, the icOn chunk rewritten.
# In the MorphOS set Carlo drew the DRAWER as Mos1 and the program badge as
# Mos2, the other way round from his other sets: the first 0.0.94 build shipped
# them swapped and the program showed up as a folder on a real MorphOS box.
"$PY" scripts/make_png_icon.py "$SRC/MOS/Mos2.info" "$OUT/morphos/TelegramAmiga.info" --project
"$PY" scripts/make_png_icon.py "$SRC/MOS/Mos1.info" "$OUT/morphos/drawer.info" --drawer
"$PY" scripts/make_png_icon.py "$SRC/AROS/AROS-1.info" "$OUT/aros/TelegramAmiga.info" --project
"$PY" scripts/make_png_icon.py "$SRC/AROS/AROS-2.info" "$OUT/aros/drawer.info" --drawer

# The TUI icon is the GUI icon under another name (byte-identical, by rule).
for lane in amigaos4 amigaos3 amigaos3-68000 morphos aros; do
    cp "$OUT/$lane/TelegramAmiga.info" "$OUT/$lane/TelegramAmiga-TUI.info"
done
echo "icons rebuilt in $OUT/{amigaos4,amigaos3,amigaos3-68000,morphos,aros}"
