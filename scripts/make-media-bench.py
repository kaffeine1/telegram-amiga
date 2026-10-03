#!/usr/bin/env python3
# Copyright (c) 2026 Michele Dipace <michele.dipace@kaffeine.net>
# SPDX-License-Identifier: MIT
"""Make the three inputs of `TelegramAmiga --media-bench <drawer>`.

  avatar.jpg  160x160 baseline JPEG, the size Telegram serves profile
              pictures in
  photo.jpg   640x480 baseline JPEG, a photo as a chat shows it
  history.gz  a gzip-packed messages.messages with 60 messages of mixed
              length, Italian text with its accents, some of them bold,
              about what one getHistory answer carries

The pictures are smooth shapes over a noisy gradient, so they compress the
way photographs do rather than the way flat drawings do. Everything comes
from fixed seeds: the same script makes the same bytes, and the bench's
checksums can be compared across machines and builds. Needs Pillow.

usage: make-media-bench.py <drawer>
"""
import gzip
import os
import random
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

VECTOR = 0x1CB5C415
MESSAGES_MESSAGES = 0x8C718E87
MESSAGE = 0x9815CEC8
PEER_USER = 0x59511722
ENTITY_BOLD = 0xBD610BC9

WORDS = (
    "ciao come stai oggi tutto bene grazie perché però città più già così "
    "caffè domani sera arrivo tardi treno lavoro casa amico amica foto "
    "messaggio Amiga Workbench disco floppy scheda rete giù là è sì"
).split()


def picture(width, height, seed):
    rnd = random.Random(seed)
    img = Image.new("RGB", (width, height))
    px = img.load()
    for y in range(height):
        for x in range(width):
            r = 120 + 100 * x // width + rnd.randint(-12, 12)
            g = 80 + 120 * y // height + rnd.randint(-12, 12)
            b = 160 - 60 * (x + y) // (width + height) + rnd.randint(-12, 12)
            px[x, y] = (max(0, min(255, r)), max(0, min(255, g)),
                        max(0, min(255, b)))
    draw = ImageDraw.Draw(img)
    for _ in range(12):
        x0 = rnd.randint(0, width - 1)
        y0 = rnd.randint(0, height - 1)
        x1 = min(width - 1, x0 + rnd.randint(width // 10, width // 3))
        y1 = min(height - 1, y0 + rnd.randint(height // 10, height // 3))
        draw.ellipse([x0, y0, x1, y1],
                     fill=(rnd.randint(0, 255), rnd.randint(0, 255),
                           rnd.randint(0, 255)))
    return img.filter(ImageFilter.GaussianBlur(1.2))


def u32(value):
    return struct.pack("<I", value)


def u64(value):
    return struct.pack("<Q", value)


def tl_string(data):
    if len(data) < 254:
        out = bytes([len(data)]) + data
    else:
        out = bytes([254]) + struct.pack("<I", len(data))[:3] + data
    return out + b"\0" * ((-len(out)) % 4)


def history(seed):
    rnd = random.Random(seed)
    messages = []
    for i in range(60):
        words = rnd.choice((3, 6, 12, 25, 60))
        text = " ".join(rnd.choice(WORDS) for _ in range(words))
        flags = 2 if i % 2 else 0  # every other one is outgoing
        entities = b""
        if i % 5 == 0:
            flags |= 128
            entities = (u32(VECTOR) + u32(1) + u32(ENTITY_BOLD) + u32(0) +
                        u32(4))
        messages.append(u32(MESSAGE) + u32(flags) + u32(0) +
                        u32(100000 - i) + u32(PEER_USER) + u64(0x12345678) +
                        u32(1790000000 - 60 * i) +
                        tl_string(text.encode("utf-8")) + entities)
    body = (u32(MESSAGES_MESSAGES) + u32(VECTOR) + u32(len(messages)) +
            b"".join(messages) + u32(VECTOR) + u32(0) + u32(VECTOR) + u32(0))
    return gzip.compress(body, compresslevel=9, mtime=0)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    drawer = sys.argv[1]
    os.makedirs(drawer, exist_ok=True)
    picture(160, 160, 1).save(os.path.join(drawer, "avatar.jpg"),
                              quality=80, progressive=False)
    picture(640, 480, 2).save(os.path.join(drawer, "photo.jpg"),
                              quality=80, progressive=False)
    with open(os.path.join(drawer, "history.gz"), "wb") as out:
        out.write(history(3))


if __name__ == "__main__":
    main()
