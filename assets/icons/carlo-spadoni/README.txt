Icon artwork by Carlo Spadoni, drawn for Telegram Amiga (September 2026) and
shipped with his consent. Kept here exactly as delivered, one drawer per
platform, "-1"/"1" the program icon and "-2"/"2" the drawer icon:

  OS4/    classic .info with an ARGB colour icon (FORM ICON), 64x64,
          normal and selected states
  OS3/    PNG artwork, 46x46 (GlowIcons size), two PNGs per file
          (normal, selected); AmigaOS 3.x cannot read PNG icons, so the
          shipped OS3 icons are built from these by scripts/make_os35_icon.py
  MOS/    PNG icons 64x64 with an icOn chunk, one state
  AROS/   PNG icons 48x47 (program) and 50x50 (drawer), two states

The launcher fields (project type, DefaultTool TelegramAmiga, 1 MB stack)
are imposed by scripts/build-icons.sh, which writes assets/icons/<lane>/.
