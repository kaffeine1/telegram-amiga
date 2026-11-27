#!/usr/bin/env python3
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Amigram's catalogs, without FlexCat or CatComp.

    python3 scripts/catalog.py header            include/tg_strings.h from locale/amigram.cd
    python3 scripts/catalog.py catalog <x.ct>    Catalogs/<language>/amigram.catalog from a translation
    python3 scripts/catalog.py all               the header and every locale/*.ct
    python3 scripts/catalog.py check             the header is up to date, every translation is sound
    python3 scripts/catalog.py text-client <dir> <dir>/<language>/amigram.catalog with only what the
                                                 text client says (the plain-68000 package's catalogs)

locale/amigram.cd is a catalog description in the CatComp/FlexCat form:
a line "MSG_NAME (id//)" and the English text on the next line(s); ";"
starts a comment. A translation locale/<language>.ct has "## version",
"## language" and "## codeset" lines, then each "MSG_NAME" and its text.
Escapes: \\n, \\t, \\\\, \\" ; a line ending in a backslash goes on. Texts are
Latin-1 in the catalog (codeset 4): the window draws one byte a letter.
The .cd and .ct files may be UTF-8 or Latin-1.

A translated text must keep the printf conversions of the English one, in
the same order: a wrong one would crash the program on the Amiga. In a
text with conversions a percent sign is written %%; in one without, %.
A requester's buttons are separated by |: a translation keeps their number.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCDIR = os.path.join(ROOT, "locale")  # not "catalogs": Catalogs/ is the same name on APFS
CD = os.path.join(SRCDIR, "amigram.cd")
HEADER = os.path.join(ROOT, "include", "tg_strings.h")
OUTDIR = os.path.join(ROOT, "Catalogs")
CATALOG_NAME = "amigram.catalog"
STATUS_WIDTH = 47  # the window's status line (TG_GUI_NAME_MAX - 1)
# the console's and the session's texts: may be turned into UTF-8 in a buffer
# of TG_LOCALE_UTF8_MAX (640) bytes
CONSOLE_PREFIXES = ("MSG_T_", "MSG_S_", "MSG_SV_")
CONSOLE_MAX = 600
# what the text client says: its own texts, the session's, and the lines of
# the login and of the reactions it shares with the window
TEXT_CLIENT_PREFIXES = ("MSG_T_", "MSG_S_", "MSG_SV_", "MSG_ST_LG_",
                        "MSG_ST_CODE_", "MSG_ST_REACT")

NAME_RE = re.compile(r"^(MSG_[A-Z0-9_]+)\s*\((\d+)/(\d*)/(\d*)\)\s*$")
CT_NAME_RE = re.compile(r"^(MSG_[A-Z0-9_]+)\s*$")
FORMAT_RE = re.compile(r"%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:l|h)?[sdiuxXcp])")


class CatalogError(Exception):
    pass


def unescape(text, where):
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            n = text[i + 1]
            if n == "n":
                out.append("\n")
            elif n == "t":
                out.append("\t")
            elif n == "\\":
                out.append("\\")
            elif n == '"':
                out.append('"')
            else:
                raise CatalogError("%s: unknown escape \\%s" % (where, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_lines(path):
    with open(path, "rb") as f:
        raw = f.read()
    try:
        text = raw.decode("utf-8")  # what today's editors write
    except UnicodeDecodeError:
        text = raw.decode("latin-1")  # an Amiga editor's file
    if "\r" in text:
        raise CatalogError("%s: CR in the file (LF only)" % path)
    return text.split("\n")


def take_text(lines, i, path):
    """The text starting at lines[i], with backslash continuations: (text, next i)."""
    parts = []
    while True:
        if i >= len(lines):
            raise CatalogError("%s: a text is missing at the end" % path)
        line = lines[i]
        i += 1
        if line.endswith("\\") and not line.endswith("\\\\"):
            parts.append(line[:-1])
            continue
        parts.append(line)
        break
    return unescape("".join(parts), "%s:%d" % (path, i)), i


def parse_cd(path=CD):
    lines = read_lines(path)
    entries = []
    names = set()
    ids = set()
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if line.startswith(";") or line.startswith("#") or not line.strip():
            continue
        m = NAME_RE.match(line)
        if not m:
            raise CatalogError("%s:%d: expected MSG_NAME (id//), got %r" % (path, i, line))
        name, ident = m.group(1), int(m.group(2))
        if name in names or ident in ids:
            raise CatalogError("%s:%d: %s or id %d twice" % (path, i, name, ident))
        text, i = take_text(lines, i, path)
        if lone_percent(text):
            raise CatalogError("%s:%d: %s has a lone %% (write %%%%)" % (path, i, name))
        names.add(name)
        ids.add(ident)
        entries.append((name, ident, text))
    if not entries:
        raise CatalogError("%s: no texts" % path)
    return entries


def parse_ct(path):
    lines = read_lines(path)
    meta = {}
    texts = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if line.startswith("##"):
            key, _, value = line[2:].strip().partition(" ")
            meta[key] = value.strip()
            continue
        if line.startswith(";") or not line.strip():
            continue
        m = CT_NAME_RE.match(line)
        if not m:
            raise CatalogError("%s:%d: expected MSG_NAME, got %r" % (path, i, line))
        name = m.group(1)
        if name in texts:
            raise CatalogError("%s:%d: %s twice" % (path, i, name))
        text, i = take_text(lines, i, path)
        texts[name] = text
    for key in ("version", "language"):
        if key not in meta:
            raise CatalogError("%s: no '## %s' line" % (path, key))
    return meta, texts


def formats(text):
    return [f for f in FORMAT_RE.findall(text) if f != "%%"]


def lone_percent(text):
    """In a text with conversions (a format), a % that is neither %% nor a
    conversion. A text without conversions is shown as written: its % is a
    percent sign (requesters double it before EasyRequest sees it)."""
    return bool(formats(text)) and "%" in FORMAT_RE.sub("", text)


def check_translation(entries, meta, texts, path):
    known = {name: text for name, _, text in entries}
    problems = []
    for name, text in texts.items():
        if name not in known:
            problems.append("%s: %s is not in amigram.cd" % (path, name))
            continue
        if lone_percent(text):
            problems.append("%s: %s has a lone %% (in a text with %%s or %%d "
                            "a percent sign is %%%%)" % (path, name))
        if name.startswith("MSG_ST_") and "\n" not in known[name]:
            # a status line holds 47 letters: no longer than that, or than
            # the English text where that is already longer
            width = len(FORMAT_RE.sub("XX", text))
            if width > max(STATUS_WIDTH, len(FORMAT_RE.sub("XX", known[name]))):
                problems.append("%s: %s is %d letters, the status line holds %d"
                                % (path, name, width, STATUS_WIDTH))
        if text.count("|") != known[name].count("|"):
            problems.append("%s: %s has %d | where the English has %d (one "
                            "button each)" % (path, name, text.count("|"),
                                              known[name].count("|")))
        if formats(text) != formats(known[name]):
            problems.append("%s: %s changes the conversions %s -> %s"
                            % (path, name, formats(known[name]), formats(text)))
        en = known[name]
        if (len(text) - len(text.lstrip("\n")) != len(en) - len(en.lstrip("\n"))
                or len(text) - len(text.rstrip("\n")) != len(en) - len(en.rstrip("\n"))):
            problems.append("%s: %s changes the line breaks at its ends"
                            % (path, name))
        # what the console and the session say may be turned into UTF-8,
        # in a buffer of 640 bytes
        if name.startswith(CONSOLE_PREFIXES) and len(text) > CONSOLE_MAX:
            problems.append("%s: %s is %d letters, a text of the console "
                            "holds %d" % (path, name, len(text), CONSOLE_MAX))
        # a text that starts or ends with a space is joined to others
        # there: the translation keeps those spaces
        if (len(text) - len(text.lstrip(" ")) != len(en) - len(en.lstrip(" "))
                or len(text) - len(text.rstrip(" ")) != len(en) - len(en.rstrip(" "))):
            problems.append("%s: %s changes the spaces at its ends (it is "
                            "joined to other text there)" % (path, name))
    missing = [name for name, _, _ in entries if name not in texts]
    return problems, missing


def c_string(text):
    out = []
    for ch in text:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append("\\%03o" % o)
    return '"' + "".join(out) + '"'


def make_header(entries):
    lines = [
        "/*",
        " * Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>",
        " * SPDX-License-Identifier: GPL-3.0-or-later",
        " */",
        "",
        "/* Generated by scripts/catalog.py from locale/amigram.cd: do not",
        "   edit, run `python3 scripts/catalog.py header`. Each text has its",
        "   catalog id and its English form, built into the program as the",
        "   fallback; TG_STR(MSG_NAME) gives the one of the user's language. */",
        "",
        "#ifndef TG_STRINGS_H",
        "#define TG_STRINGS_H",
        "",
    ]
    for name, ident, text in entries:
        lines.append("#define %s %d" % (name, ident))
        lines.append("#define %s_STR %s" % (name, c_string(text)))
    lines += ["",
              "/* the highest id above: the host build sizes its table by it */",
              "#define TG_STRINGS_ID_MAX %d" % max(i for _, i, _ in entries),
              "", "#endif", ""]
    return "\n".join(lines)


def chunk(cid, body):
    data = cid + struct.pack(">I", len(body)) + body
    if len(body) & 1:
        data += b"\0"
    return data


def make_catalog(entries, meta, texts):
    by_name = {name: ident for name, ident, _ in entries}
    version = meta["version"]
    if not version.startswith("$VER: "):
        version = "$VER: " + version
    language = meta["language"]
    codeset = int(meta.get("codeset", "4"))
    strs = b""
    for name, _, _ in entries:
        if name not in texts:
            continue  # the English one, built in, stands
        try:
            raw = texts[name].encode("latin-1") + b"\0"
        except UnicodeEncodeError:
            raise CatalogError("%s has a letter outside Latin-1" % name)
        strs += struct.pack(">II", by_name[name], len(raw)) + raw
        strs += b"\0" * ((4 - len(raw) % 4) % 4)
    body = b"CTLG"
    body += chunk(b"FVER", version.encode("latin-1") + b"\0")
    body += chunk(b"LANG", language.encode("latin-1") + b"\0")
    body += chunk(b"CSET", struct.pack(">I", codeset) + b"\0" * 28)
    body += chunk(b"STRS", strs)
    return language, b"FORM" + struct.pack(">I", len(body)) + body


def read_catalog(data):
    """(language, {id: text}) of a catalog, to check what was written."""
    if data[:4] != b"FORM" or data[8:12] != b"CTLG":
        raise CatalogError("not a catalog")
    end = 8 + struct.unpack(">I", data[4:8])[0]
    i = 12
    language = None
    strings = {}
    while i < end:
        cid = data[i:i + 4]
        size = struct.unpack(">I", data[i + 4:i + 8])[0]
        body = data[i + 8:i + 8 + size]
        if cid == b"LANG":
            language = body.rstrip(b"\0").decode("latin-1")
        elif cid == b"STRS":
            j = 0
            while j < size:
                ident, length = struct.unpack(">II", body[j:j + 8])
                raw = body[j + 8:j + 8 + length]
                strings[ident] = raw.rstrip(b"\0").decode("latin-1")
                j += 8 + ((length + 3) & ~3)
        i += 8 + size + (size & 1)
    return language, strings


def write_catalog(entries, ct_path, outdir=None, prefixes=None):
    meta, texts = parse_ct(ct_path)
    problems, missing = check_translation(entries, meta, texts, ct_path)
    if problems:
        raise CatalogError("\n".join(problems))
    if prefixes is not None:
        texts = {n: t for n, t in texts.items() if n.startswith(prefixes)}
    language, data = make_catalog(entries, meta, texts)
    lang2, strings = read_catalog(data)
    by_name = {name: ident for name, ident, _ in entries}
    if lang2 != language or any(strings[by_name[n]] != t for n, t in texts.items()):
        raise CatalogError("%s: the catalog does not read back" % ct_path)
    out = os.path.join(outdir or OUTDIR, language, CATALOG_NAME)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    return out, len(texts), missing


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    entries = parse_cd()
    cmd = argv[1]
    if cmd in ("header", "all"):
        with open(HEADER, "w") as f:
            f.write(make_header(entries))
        print("%s: %d texts" % (os.path.relpath(HEADER, ROOT), len(entries)))
    if cmd in ("catalog", "all"):
        cts = argv[2:] if cmd == "catalog" else sorted(
            os.path.join(SRCDIR, n) for n in os.listdir(SRCDIR)
            if n.endswith(".ct"))
        for ct in cts:
            out, n, missing = write_catalog(entries, ct)
            print("%s: %d of %d texts translated%s" % (
                os.path.relpath(out, ROOT), n, len(entries),
                "" if not missing else " (missing: %s)" % ", ".join(missing[:8])
                + (" ..." if len(missing) > 8 else "")))
    if cmd == "text-client":
        if len(argv) < 3:
            print(__doc__)
            return 2
        for n in sorted(os.listdir(SRCDIR)):
            if n.endswith(".ct"):
                out, count, _ = write_catalog(
                    entries, os.path.join(SRCDIR, n), outdir=argv[2],
                    prefixes=TEXT_CLIENT_PREFIXES)
                print("%s: %d texts of the text client" % (out, count))
    if cmd == "check":
        with open(HEADER) as f:
            if f.read() != make_header(entries):
                print("include/tg_strings.h is out of date: run catalog.py header")
                return 1
        bad = 0
        for n in sorted(os.listdir(SRCDIR)):
            if n.endswith(".ct"):
                path = os.path.join(SRCDIR, n)
                meta, texts = parse_ct(path)
                problems, missing = check_translation(entries, meta, texts, path)
                for p in problems:
                    print(p)
                bad += len(problems)
                if not problems:
                    language, data = make_catalog(entries, meta, texts)
                    out = os.path.join(OUTDIR, language, CATALOG_NAME)
                    try:
                        with open(out, "rb") as f:
                            same = f.read() == data
                    except OSError:
                        same = False
                    if not same:
                        print("%s is out of date: run catalog.py all"
                              % os.path.relpath(out, ROOT))
                        bad += 1
                print("%s: %d of %d translated" % (n, len(texts), len(entries)))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except CatalogError as e:
        print("catalog.py: %s" % e, file=sys.stderr)
        sys.exit(1)
