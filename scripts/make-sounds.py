#!/usr/bin/env python3
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
#
# Builds the notification sounds shipped in the GUI packages (sounds/):
# short IFF 8SVX samples synthesised here from sines, so they carry no
# third-party rights and any system's 8svx.datatype plays them, a stock
# AmigaOS 3.x included. Run it to regenerate assets/sounds/:
#
#   python3 scripts/make-sounds.py assets/sounds

import math
import os
import struct
import sys

RATE = 22050


def bell(notes, note_len, decay, overtone=0.3):
    out = []
    for f in notes:
        n = int(RATE * note_len)
        for i in range(n):
            t = i / RATE
            env = math.exp(-decay * t) * min(1.0, i / (RATE * 0.003))
            v = (math.sin(2 * math.pi * f * t) +
                 overtone * math.sin(4 * math.pi * f * t)) / (1 + overtone)
            out.append(0.8 * env * v)
    return out


def sweep(f0, f1, length, decay):
    out = []
    n = int(RATE * length)
    phase = 0.0
    for i in range(n):
        t = i / RATE
        f = f0 + (f1 - f0) * (i / n)
        phase += 2 * math.pi * f / RATE
        env = math.exp(-decay * t) * min(1.0, i / (RATE * 0.002))
        out.append(0.8 * env * math.sin(phase))
    return out


def marimba(notes, note_len):
    out = []
    for f in notes:
        n = int(RATE * note_len)
        for i in range(n):
            t = i / RATE
            body = math.exp(-9.0 * t) * math.sin(2 * math.pi * f * t)
            knock = 0.4 * math.exp(-40.0 * t) * math.sin(2 * math.pi * 4 * f * t)
            env = min(1.0, i / (RATE * 0.002))
            out.append(0.75 * env * (body + knock))
    return out


SOUNDS = {
    "Ding": bell([1047, 1568], 0.25, 6.0),
    "Blip": bell([660, 880, 1320], 0.09, 12.0),
    "Bell": bell([880, 1175, 1760], 0.22, 6.0),
    "Pop": sweep(900, 300, 0.08, 20.0),
    "Drop": sweep(400, 1600, 0.12, 18.0),
    "Marimba": marimba([523, 659], 0.22),
}


def write_8svx(path, samples, name):
    body = bytes((int(round(max(-1.0, min(1.0, v)) * 127)) & 0xFF)
                 for v in samples)
    if len(body) % 2:
        body += b"\0"
    vhdr = struct.pack(">IIIHBBI", len(samples), 0, 0, RATE, 1, 0, 0x10000)
    label = name.encode("ascii") + b"\0"
    if len(label) % 2:
        label += b"\0"
    chunks = (b"VHDR" + struct.pack(">I", len(vhdr)) + vhdr +
              b"NAME" + struct.pack(">I", len(label)) + label +
              b"BODY" + struct.pack(">I", len(body)) + body)
    with open(path, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", 4 + len(chunks)) + b"8SVX" + chunks)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/sounds"
    os.makedirs(out, exist_ok=True)
    for name, samples in SOUNDS.items():
        path = os.path.join(out, name + ".8svx")
        write_8svx(path, samples, name)
        print("%s %d bytes" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    main()
