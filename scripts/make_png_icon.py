#!/usr/bin/env python3
"""Impose our launcher fields on a PNG icon (MorphOS, AROS, OS4 read those).

A PNG icon is one PNG, or two concatenated (normal and selected), and the
first one carries an "icOn" chunk: a list of attributes, each a big-endian
ULONG tag followed by a ULONG value or a NUL-terminated string. This script
replaces that chunk with the fields a TelegramAmiga launcher icon needs
(type, DefaultTool, stack, an optional drawer geometry) and leaves every
pixel and every other chunk untouched, CRC recomputed for the new chunk.

Tags as icon.library reads them (AROS workbench/libs/icon/diskobjPNGio.c):
  0x80001009 STACKSIZE   ULONG      0x8000100a DEFAULTTOOL  string
  0x8000100b TOOLTYPE    string     0x8000100f TYPE         ULONG (WB*)
  0x80001003 DRAWERX .. 0x80001006 DRAWERHEIGHT, 0x80001007 DRAWERFLAGS,
  0x8000100c VIEWMODES                                      ULONG
An unknown tag stops the reader, so nothing else is written.

Usage:
  make_png_icon.py IN.info OUT.info --project --tool TelegramAmiga --stack 1048576
  make_png_icon.py IN.info OUT.info --drawer
  make_png_icon.py --dump IN.info
"""
import struct
import sys
import zlib

SIG = b"\x89PNG\r\n\x1a\n"
TAGS = {"ICONX": 0x80001001, "ICONY": 0x80001002, "DRAWERX": 0x80001003,
        "DRAWERY": 0x80001004, "DRAWERWIDTH": 0x80001005,
        "DRAWERHEIGHT": 0x80001006, "DRAWERFLAGS": 0x80001007,
        "TOOLWINDOW": 0x80001008, "STACKSIZE": 0x80001009,
        "DEFAULTTOOL": 0x8000100a, "TOOLTYPE": 0x8000100b,
        "VIEWMODES": 0x8000100c, "DD_CURRENTX": 0x8000100d,
        "DD_CURRENTY": 0x8000100e, "TYPE": 0x8000100f, "FRAMELESS": 0x80001010,
        "DRAWERFLAGS3": 0x80001011, "VIEWMODES2": 0x80001012,
        "DRAWERFLAGS2": 0x80001107}
NAMES = dict((v, k) for k, v in TAGS.items())
STRINGS = (0x80001008, 0x8000100a, 0x8000100b)
WBDRAWER, WBPROJECT = 2, 4


def split_pngs(data):
    """[(start, [(type, body), ...]), ...] for each concatenated PNG."""
    pngs = []
    p = 0
    while data[p:p + 8] == SIG:
        q = p + 8
        chunks = []
        while q + 12 <= len(data):
            ln = struct.unpack(">I", data[q:q + 4])[0]
            ctype = data[q + 4:q + 8]
            chunks.append((ctype, data[q + 8:q + 8 + ln]))
            q += 12 + ln
            if ctype == b"IEND":
                break
        pngs.append((p, chunks))
        p = q
    if p != len(data):
        raise ValueError("trailing bytes after the last PNG")
    return pngs


def chunk(ctype, body):
    return (struct.pack(">I", len(body)) + ctype + body +
            struct.pack(">I", zlib.crc32(ctype + body) & 0xffffffff))


def parse_icon_attrs(body):
    attrs = []
    i = 0
    while i + 4 <= len(body):
        tag = struct.unpack(">I", body[i:i + 4])[0]
        i += 4
        if tag in STRINGS:
            e = body.index(b"\x00", i)
            attrs.append((tag, body[i:e].decode("latin-1")))
            i = e + 1
        else:
            attrs.append((tag, struct.unpack(">I", body[i:i + 4])[0]))
            i += 4
    return attrs


def build_icon_attrs(attrs):
    out = b""
    for tag, val in attrs:
        out += struct.pack(">I", tag)
        if tag in STRINGS:
            out += val.encode("latin-1") + b"\x00"
        else:
            out += struct.pack(">I", val & 0xffffffff)
    return out


def describe(data):
    pngs = split_pngs(data)
    lines = []
    for start, chunks in pngs:
        ihdr = [b for t, b in chunks if t == b"IHDR"][0]
        w, h = struct.unpack(">II", ihdr[:8])
        icon = [b for t, b in chunks if t == b"icOn"]
        desc = "%dx%d" % (w, h)
        if icon:
            desc += " icOn " + ", ".join(
                "%s=%r" % (NAMES.get(t, hex(t)), v) for t, v in parse_icon_attrs(icon[0]))
        lines.append(desc)
    return "%d PNG: " % len(pngs) + " | ".join(lines)


def rewrite(data, attrs):
    pngs = split_pngs(data)
    out = b""
    for n, (start, chunks) in enumerate(pngs):
        out += SIG
        placed = False
        for ctype, body in chunks:
            if ctype == b"icOn":
                continue  # the old one goes, wherever it was
            if n == 0 and not placed and ctype in (b"IDAT", b"IEND"):
                out += chunk(b"icOn", build_icon_attrs(attrs))
                placed = True
            out += chunk(ctype, body)
    return out


def main(argv):
    flags = [a for a in argv[1:] if a.startswith("--")]
    args = [a for a in argv[1:] if not a.startswith("--")]
    if "--dump" in flags and len(args) == 1:
        print("%s: %s" % (args[0], describe(open(args[0], "rb").read())))
        return 0
    if len(args) < 2 or not ({"--project", "--drawer"} & set(flags)):
        sys.stderr.write(__doc__)
        return 2
    tool = "TelegramAmiga"
    stack = 1048576
    for i, a in enumerate(argv):
        if a == "--tool":
            tool = argv[i + 1]
        if a == "--stack":
            stack = int(argv[i + 1])
    args = [a for a in args if a not in (tool, str(stack))]
    src, dst = args[0], args[1]
    data = open(src, "rb").read()
    if "--drawer" in flags:
        attrs = [(TAGS["TYPE"], WBDRAWER), (TAGS["STACKSIZE"], stack),
                 (TAGS["DRAWERX"], 20), (TAGS["DRAWERY"], 20),
                 (TAGS["DRAWERWIDTH"], 600), (TAGS["DRAWERHEIGHT"], 199),
                 (TAGS["DRAWERFLAGS"], 0), (TAGS["VIEWMODES"], 0)]
    else:
        attrs = [(TAGS["TYPE"], WBPROJECT), (TAGS["STACKSIZE"], stack),
                 (TAGS["DEFAULTTOOL"], tool)]
    out = rewrite(data, attrs)
    # Read back: same pixels (every non-icOn chunk identical), our fields only.
    before = [[(t, b) for t, b in c if t != b"icOn"] for _, c in split_pngs(data)]
    after = split_pngs(out)
    if ([[(t, b) for t, b in c if t != b"icOn"] for _, c in after] != before or
            parse_icon_attrs([b for t, b in after[0][1] if t == b"icOn"][0]) != attrs):
        sys.stderr.write("FATAL: the rewritten icon does not read back as intended\n")
        return 1
    open(dst, "wb").write(out)
    print("wrote %s (%d bytes): %s" % (dst, len(out), describe(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
