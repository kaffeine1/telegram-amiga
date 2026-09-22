#!/usr/bin/env python3
"""Build a classic AmigaOS 3.5 colour icon (.info) from PNG artwork.

AmigaOS 3.x cannot read PNG icons (icon.library 47.5 of 3.2.3 included), so
artwork drawn as PNG has to become the format 3.5 introduced: the classic
DiskObject with a planar image for every Workbench since 1.x, followed by a
FORM ICON with a FACE chunk and one IMAG chunk per state, palette based, RLE
compressed, up to 256 colours. This is the "GlowIcons" format, and what the
stock 3.2 icons are. The planar image here is a Floyd-Steinberg rendition in
the four default Workbench pens, so a plain 3.1 shows a sensible icon too.

Input is a PNG with an alpha channel (or two PNGs concatenated in one file,
normal then selected, as some icon tools save them), or an OS4 icon whose
ARGB chunks carry the states; --selected names a separate second state. Fully transparent pixels become the transparent colour.

The FORM ICON layout written (verified against the 3.2 CD icons):
  FACE  width-1, height-1, flags (bit0 frameless), aspect (x<<4|y), UWORD
        max palette bytes - 1 over the states
  IMAG  transparent colour, colours-1, flags (bit0 transparent, bit1 palette),
        image format 1 (RLE), palette format 1 (RLE), depth (bits per pixel),
        UWORD image bytes - 1, UWORD palette bytes - 1, then the two streams.
  RLE   a bit stream: 8-bit control c, then c+1 literal values of `depth` bits
        when c < 128, or one value repeated 257-c times when c > 128.

Needs Pillow. The launcher fields follow scripts/make_gui_icon.py.

Usage:
  make_os35_icon.py IN.png OUT.info --project [--tool TelegramAmiga] [--stack N]
  make_os35_icon.py IN.png OUT.info --drawer
  options: --selected SEL.png  --colors 64  --frameless  --matte 170,170,170
           --alpha-cut 32  --no-argb
The rim of the drawing is blended over --matte (the Workbench grey) since a
palette icon has no alpha; pixels under --alpha-cut stay transparent. Unless
--no-argb, the file also carries the drawing with its alpha as OS4 ARGB
chunks, which icon.library versions that know them blend over any backdrop.
"""
import io
import struct
import zlib
import sys

from PIL import Image

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
import make_gui_icon as classic  # noqa: E402  (the DiskObject walker)

WB_PENS = [(0x95, 0x95, 0x95), (0x00, 0x00, 0x00), (0xff, 0xff, 0xff), (0x3b, 0x67, 0xa2)]
NO_ICON_POSITION = 0x80000000
SIG = b"\x89PNG\r\n\x1a\n"


class BitWriter(object):
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            chunk = self.bits[i:i + 8] + [0] * (8 - len(self.bits[i:i + 8]))
            out.append(int("".join(str(b) for b in chunk), 2))
        return bytes(out)


class BitReader(object):
    def __init__(self, data):
        self.d, self.pos = data, 0

    def get(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v


def rle_encode(values, depth):
    w = BitWriter()
    i, n = 0, len(values)
    while i < n:
        run = 1
        while i + run < n and values[i + run] == values[i] and run < 128:
            run += 1
        if run >= 2:
            w.put(257 - run, 8)
            w.put(values[i], depth)
            i += run
            continue
        j = i
        while j < n and j - i < 128:
            if j + 1 < n and values[j + 1] == values[j] and (j + 2 < n and values[j + 2] == values[j]):
                break
            j += 1
        w.put(j - i - 1, 8)
        for v in values[i:j]:
            w.put(v, depth)
        i = j
    return w.bytes()


def rle_decode(data, depth, count):
    r = BitReader(data)
    out = []
    while len(out) < count and (r.pos >> 3) < len(data):
        c = r.get(8)
        if c < 128:
            for _ in range(c + 1):
                if len(out) >= count:
                    break
                out.append(r.get(depth))
        elif c > 128:
            v = r.get(depth)
            out.extend([v] * (257 - c))
    return out[:count]


def split_pngs(data):
    parts = []
    p = 0
    while data[p:p + 8] == SIG:
        q = p + 8
        while q + 12 <= len(data):
            ln = struct.unpack(">I", data[q:q + 4])[0]
            ctype = data[q + 4:q + 8]
            q += 12 + ln
            if ctype == b"IEND":
                break
        parts.append(data[p:q])
        p = q
    return parts


def argb_states(data):
    """The states of an OS4 icon: every ARGB chunk of its FORM ICON decoded
    to an RGBA image (the inverse of argb_chunk_body)."""
    i = data.find(b"FORM")
    if i < 0 or data[i + 8:i + 12] != b"ICON":
        return []
    p = i + 12
    w = h = None
    images = []
    while p + 8 <= len(data):
        cid = data[p:p + 4]
        ln = struct.unpack(">I", data[p + 4:p + 8])[0]
        body = data[p + 8:p + 8 + ln]
        if cid == b"FACE":
            w, h = body[0] + 1, body[1] + 1
        elif cid == b"ARGB" and w:
            raw = zlib.decompress(body[10:])
            rgba = bytearray(len(raw))
            rgba[0::4] = raw[1::4]
            rgba[1::4] = raw[2::4]
            rgba[2::4] = raw[3::4]
            rgba[3::4] = raw[0::4]
            images.append(Image.frombytes("RGBA", (w, h), bytes(rgba)))
        p += 8 + ln + (ln & 1)
    return images


def load_states(path, selected):
    data = open(path, "rb").read()
    parts = split_pngs(data)
    if not parts:
        # not PNG artwork: an OS4 icon drawn with its alpha (ARGB chunks)
        # serves just as well, and is what the 64 pixel sets are
        images = argb_states(data)
        if not images:
            raise ValueError("%s is neither a PNG nor an ARGB icon" % path)
        if selected:
            images = images[:1] + [Image.open(selected).convert("RGBA")]
        return images
    images = [Image.open(io.BytesIO(parts[0])).convert("RGBA")]
    if selected:
        images.append(Image.open(selected).convert("RGBA"))
    elif len(parts) > 1:
        images.append(Image.open(io.BytesIO(parts[1])).convert("RGBA"))
    return images


def matte_over(im, matte):
    """The artwork over the Workbench background colour: a palette icon has
    no alpha channel, so the antialiased rim of the drawing has to be blended
    here, once, against the grey it will most likely sit on. Without this the
    rim pixels keep their straight colour and the icon gets a dark, ragged
    edge (a tester saw exactly that on the first 0.0.94 build)."""
    bg = Image.new("RGBA", im.size, matte + (255,))
    return Image.alpha_composite(bg, im).convert("RGB")


def quantize_state(im, colors, matte, alpha_cut):
    """(indices, palette): index 0 is the transparent colour, the rest the
    median-cut palette of the pixels that show, matted over `matte`."""
    w, h = im.size
    alpha = im.split()[3]
    opaque = matte_over(im, matte)
    q = opaque.quantize(colors=colors - 1, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    qpal = q.getpalette()[:3 * (colors - 1)]
    qdata = list(q.tobytes())
    used = sorted(set(qdata))
    remap = dict((old, new + 1) for new, old in enumerate(used))
    palette = [WB_PENS[0]] + [tuple(qpal[3 * k:3 * k + 3]) for k in used]
    idx = []
    adata = list(alpha.tobytes())
    for k in range(w * h):
        idx.append(0 if adata[k] < alpha_cut else remap[qdata[k]])
    return idx, palette


def argb_chunk_body(im):
    """The OS4 way to carry the drawing with its alpha: A,R,G,B per pixel,
    zlib compressed, behind a ten byte header (a one, the compressed size
    minus one, a zero word: as the OS4 icons on disk have it). icon.library
    versions that know it (AmigaOS 4, the third-party one AmiKit ships) blend
    it over any backdrop; the 3.5 one of stock AmigaOS 3.x skips the chunk and
    draws the palette image above."""
    rgba = im.convert("RGBA").tobytes()
    argb = bytearray(len(rgba))
    argb[0::4] = rgba[3::4]
    argb[1::4] = rgba[0::4]
    argb[2::4] = rgba[1::4]
    argb[3::4] = rgba[2::4]
    z = zlib.compress(bytes(argb), 9)
    return struct.pack(">IIH", 1, len(z) - 1, 0) + z


def planar(im, matte, alpha_cut):
    """Two bitplanes in the four Workbench pens, dithered; transparent = pen 0."""
    w, h = im.size
    pal = Image.new("P", (1, 1))
    pal.putpalette(sum([list(c) for c in WB_PENS], []) + [0] * (768 - 12))
    q = matte_over(im, matte).quantize(palette=pal, dither=Image.Dither.FLOYDSTEINBERG)
    alpha = list(im.split()[3].tobytes())
    pens = [0 if alpha[k] < alpha_cut else v for k, v in enumerate(q.tobytes())]
    rowbytes = ((w + 15) // 16) * 2
    planes = bytearray()
    for plane in range(2):
        for y in range(h):
            row = bytearray(rowbytes)
            for x in range(w):
                if (pens[y * w + x] >> plane) & 1:
                    row[x >> 3] |= 0x80 >> (x & 7)
            planes += row
    return bytes(planes)


def chunk(cid, body):
    return cid + struct.pack(">I", len(body)) + body + (b"\x00" if len(body) & 1 else b"")


def build(images, do_type, tool, stack, colors, frameless, drawer, matte, alpha_cut, argb):
    w, h = images[0].size
    for im in images[1:]:
        if im.size != (w, h):
            raise ValueError("the selected state must have the same size")
    states = [quantize_state(im, colors, matte, alpha_cut) for im in images]
    imags = []
    for idx, palette in states:
        depth = max(1, (len(palette) - 1).bit_length())
        img = rle_encode(idx, depth)
        palbytes = [c for rgb in palette for c in rgb]
        pal = rle_encode(palbytes, 8)
        # what we wrote must decode to what we meant
        assert rle_decode(img, depth, len(idx)) == idx
        assert rle_decode(pal, 8, len(palbytes)) == palbytes
        body = struct.pack(">BBBBBBHH", 0, len(palette) - 1, 3, 1, 1, depth, len(img) - 1, len(pal) - 1) + img + pal
        imags.append((body, len(palbytes)))
    face = struct.pack(">BBBBH", w - 1, h - 1, 1 if frameless else 0, 0x11, max(n for _, n in imags) - 1)
    form_body = b"ICON" + chunk(b"FACE", face) + b"".join(chunk(b"IMAG", b) for b, _ in imags)
    if argb:
        form_body += b"".join(chunk(b"ARGB", argb_chunk_body(im)) for im in images)
    form = b"FORM" + struct.pack(">I", len(form_body)) + form_body
    # classic part
    has_select = len(images) > 1
    gadget = struct.pack(">IhhhhHHHIIIiIhI", 0, 0, 0, w, h, 6 if has_select else 4, 3, 1,
                         1, 1 if has_select else 0, 0, 0, 0, 0, 1)
    header = (b"\xe3\x10\x00\x01" + gadget + bytes([do_type, 0]) +
              struct.pack(">IIIIIII", 1 if tool else 0, 0, NO_ICON_POSITION, NO_ICON_POSITION,
                          1 if drawer else 0, 0, stack))
    out = header
    if drawer:
        out += classic.DEFAULT_DRAWERDATA
    for im in images:
        out += struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 3, 0, 0) + planar(im, matte, alpha_cut)
    if tool:
        s = tool.encode("latin-1") + b"\x00"
        out += struct.pack(">I", len(s)) + s
    if drawer:
        out += classic.DEFAULT_DRAWERDATA2
    return out + form


def main(argv):
    flags = [a for a in argv[1:] if a.startswith("--")]
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = {}
    for i, a in enumerate(argv):
        if a in ("--tool", "--stack", "--selected", "--colors", "--matte", "--alpha-cut") and i + 1 < len(argv):
            opts[a] = argv[i + 1]
    args = [a for a in args if a not in opts.values()]
    if len(args) < 2 or not ({"--project", "--drawer"} & set(flags)):
        sys.stderr.write(__doc__)
        return 2
    drawer = "--drawer" in flags
    tool = None if drawer else opts.get("--tool", "TelegramAmiga")
    stack = int(opts.get("--stack", "1048576"))
    colors = int(opts.get("--colors", "64"))
    if not 2 <= colors <= 256:
        sys.stderr.write("colors must be 2..256\n")
        return 2
    matte = tuple(int(v) for v in opts.get("--matte", "170,170,170").split(","))
    if len(matte) != 3 or not all(0 <= v <= 255 for v in matte):
        sys.stderr.write("matte must be R,G,B\n")
        return 2
    alpha_cut = int(opts.get("--alpha-cut", "32"))
    images = load_states(args[0], opts.get("--selected"))
    out = build(images, classic.WBDRAWER if drawer else classic.WBPROJECT, tool, stack,
                colors, "--frameless" in flags, drawer, matte, alpha_cut, "--no-argb" not in flags)
    back = classic.parse(out)
    if classic.serialize(back) != out:
        sys.stderr.write("FATAL: the icon does not walk back as written\n")
        return 1
    open(args[1], "wb").write(out)
    print("wrote %s (%d bytes, %d state%s, %d colours max): %s"
          % (args[1], len(out), len(images), "s" if len(images) > 1 else "", colors,
             classic.describe(back)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
