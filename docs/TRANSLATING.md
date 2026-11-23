# Translating Amigram

Amigram speaks the user's language through the system's locale.library,
as every Amiga program does: the English texts are built into the
program, and a catalog per language, in `Catalogs/<language>/` next to
the program, replaces them. The Locale preferences choose the language.

## The files

- `locale/amigram.cd`: every text, with its id and its English form.
  Ids never change once a release has them; a new text takes a new id.
- `locale/<language>.ct`: one translation, for example
  `locale/italiano.ct`. The language name is the one locale.library
  uses (`italiano`, `deutsch`, `nederlands`, `svenska`, ...).
- `scripts/catalog.py`: turns them into `include/tg_strings.h` and
  `Catalogs/<language>/amigram.catalog`. Python 3 is all it needs; no
  FlexCat or CatComp.

## A new language

1. Copy `locale/italiano.ct` to `locale/<language>.ct` and change the
   `## language` line (and the name in `## version`).
2. Under each `MSG_...` line write the text in your language. A line
   starting with `;` is a comment: the English text is kept there as a
   reminder. `\n` is a line break, and a line ending in a backslash goes
   on in the next one.
3. Keep every `%s`, `%lu`, `%d` of the English text, in the same order:
   a missing or different one would crash the program. In a text that
   has them, a percent sign is written `%%`; in a text without any, `%`.
   Requester buttons are separated by `|`: keep their number and order.
4. Keep status lines (the `MSG_ST_` texts) within 47 letters, or no
   longer than the English where that is already longer: the window's
   status line holds no more.
5. Run `python3 scripts/catalog.py all`. It refuses a text that changes
   the conversions, the number of buttons or the status line's width,
   and says which texts are still missing (those stay English).

The file can be UTF-8 or Latin-1; the catalog is Latin-1, the letters the
window draws today. Languages written with other letters (Polish, Czech,
Greek, Russian) wait for the window to draw them (see ROADMAP.md,
"Letters outside Latin-1").

## Trying it

Put `Catalogs/<language>/amigram.catalog` in the Amigram drawer, choose
the language in the Locale preferences and start Amigram. On the host
build, `TG_CATALOG=Catalogs/<language>/amigram.catalog build/host/Amigram
--gui-self-test` runs the window's layout checks with the translated
texts: a label too wide for its place shows up there.

## A new text in the code

Add it to `locale/amigram.cd` with a new id, run
`python3 scripts/catalog.py all`, and use `TG_STR(MSG_NAME)` (from
`tg_locale.h`) where the code had the English string. Copy it with a
bounded copy, never strcpy: a translation can be longer. A text with a
number or a name goes through `tg_locale_format(buf, sizeof(buf),
TG_STR(MSG_NAME), ...)`, which cuts what does not fit; build once with
`-DTG_LOCALE_CHECK_FORMATS` to have the compiler check the arguments
against the English text. A plural is two texts (`MSG_X_1` and `MSG_X`),
not an "s" added in the code. Requesters show their text as written.
