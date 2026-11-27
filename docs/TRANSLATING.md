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
5. Keep the spaces and the line breaks a text starts and ends with: it
   is joined to another text there, or closes a line of the console.
   `MSG_SV_X` and `MSG_SV_X_YOU` are a service line said of someone
   (`%s` is the name) and of the reader: "Anna pinned a message", "You
   pinned a message". Put the words in the order your language wants.
6. Run `python3 scripts/catalog.py all`. It refuses a text that changes
   the conversions, the number of buttons, the spaces or line breaks at
   its ends, the status line's width, or the 600 letters a text of the
   console and of the session may have (`MSG_T_`, `MSG_S_`, `MSG_SV_`),
   and says which texts are still missing (those stay English).

The file can be UTF-8 or Latin-1; the catalog is Latin-1, the letters the
window draws today. Languages written with other letters (Polish, Czech,
Greek, Russian) wait for the window to draw them (see ROADMAP.md,
"Letters outside Latin-1").

## What is translated

The window, the text client (the plain-68000 package included) and what
the session says in both: media labels such as `[Photo]`, polls, service
lines, "Saved Messages", the reasons of a failed login. Technical lines
written for a bug report (the names Telegram gives its errors, the debug
log, the self-tests, the command line's own messages) stay English, and
so do the commands of the text client (`/list`, `/react heart`).

## Trying it

Put `Catalogs/<language>/amigram.catalog` in the Amigram drawer, choose
the language in the Locale preferences and start Amigram. On the host
build, `TG_CATALOG=Catalogs/<language>/amigram.catalog build/host/Amigram
--gui-self-test` runs the window's layout checks with the translated
texts: a label too wide for its place shows up there.
`--mtproto-self-test` with a catalog first checks what the session says
in that language (every service line fits and ends on a whole letter),
then runs its English checks. `python3 scripts/catalog.py text-client
<dir>` writes the smaller catalogs of the 68000 package, with only what
the text client says.

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

Three forms of the same text, by where it goes:

- `TG_STR(MSG_NAME)`: Latin-1, for the window and its status line.
- `TG_STR_U8(MSG_NAME)`: UTF-8, for what is joined to Telegram's own
  text before the window or the console converts it: a media label, a
  service line, a chat's name.
- `TG_TSTR(MSG_NAME)` in the text client (`TG_ATXT` in tg_app.c): the
  text made ready for the console, Latin-1 on an Amiga console and
  UTF-8 under `--charset utf8`. `fprintf(stream, TG_TSTR(MSG_X), ...)`
  is fine for a stream; a text with no argument goes out with `fputs`.

Never hand `MSG_NAME` on through a macro of your own to one of these:
the preprocessor would replace it by its number first. Texts that the
self-tests compare are compared in English: the session's suites shut
the catalog before they run.
