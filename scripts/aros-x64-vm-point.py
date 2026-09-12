#!/usr/bin/env python3
"""Closed-loop pointing for the AROS x86_64 QEMU VM (VNC 127.0.0.1::5911).

Why this exists: qemu offers a usb-tablet, but the AROS x86_64 guest drives the
PS/2 mouse instead, so it sees RELATIVE motion.  Absolute vnc coordinates are
therefore NOT 1:1 with the guest pointer: a long move lands about 5% short, the
error is not a fixed offset, and dragging a window title bar simply does not
work.  The way that does work is to look at the screen: slam the pointer into
the top-left corner (the guest clamps there, so we know where it is), walk to
the target in small paced hops, then screenshot, find the red arrow and correct
until its tip sits on the target.

Two more notes about this VM, learned the hard way:
  * an AROS-Shell window covering the desktop icons is best shrunk with its own
    ZOOM gadget (one click) rather than dragged out of the way;
  * the guest keyboard layout is Italian: '/' is shift-7 and '-' is the key
    that carries '/' on a US board.  A shifted character sent as a single
    character does not come out shifted, so build key names explicitly.

Usage:
  aros-x64-vm-point.py click X Y            # land on (X,Y) and left click
  aros-x64-vm-point.py double X Y           # ... and double click
  aros-x64-vm-point.py move X Y             # just land there
  aros-x64-vm-point.py shot FILE            # capture the screen
  aros-x64-vm-point.py type TEXT            # type with the Italian mapping

As a module:
  from aros_x64_vm_point import Pointer
  p = Pointer(); p.land(753, 545); p.click(); p.shot('panel.png'); p.close()
"""
import sys
import time

from PIL import Image
from vncdotool import api

VNC = "127.0.0.1::5911"
SHOT = "/tmp/aros-x64-point.png"

# the arrow is a bright red blob roughly 14x22; every other red thing on screen
# (dock icons, a requester's cancel X, emoji) has a different size
ARROW_W = (9, 20)
ARROW_H = (15, 28)
ARROW_MIN_PIXELS = 55


def _is_red(p):
    return p[0] > 170 and p[1] < 80 and p[2] < 80


def find_arrow(png, roi):
    """Top-left corner of the arrow inside roi=(x0,y0,x1,y1), or None."""
    im = Image.open(png).convert("RGB")
    x0, y0 = max(0, roi[0]), max(0, roi[1])
    x1, y1 = min(im.width, roi[2]), min(im.height, roi[3])
    px = im.load()
    seen = set()
    best = None
    for y in range(y0, y1):
        for x in range(x0, x1):
            if (x, y) in seen or not _is_red(px[x, y]):
                continue
            stack = [(x, y)]
            seen.add((x, y))
            blob = []
            while stack:
                cx, cy = stack.pop()
                blob.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1),
                               (cx, cy - 1), (cx + 1, cy + 1), (cx - 1, cy - 1),
                               (cx + 1, cy - 1), (cx - 1, cy + 1)):
                    if (x0 <= nx < x1 and y0 <= ny < y1
                            and (nx, ny) not in seen and _is_red(px[nx, ny])):
                        seen.add((nx, ny))
                        stack.append((nx, ny))
            bx0 = min(p[0] for p in blob)
            by0 = min(p[1] for p in blob)
            w = max(p[0] for p in blob) - bx0 + 1
            h = max(p[1] for p in blob) - by0 + 1
            if (ARROW_W[0] <= w <= ARROW_W[1] and ARROW_H[0] <= h <= ARROW_H[1]
                    and len(blob) >= ARROW_MIN_PIXELS):
                if best is None or len(blob) > best[2]:
                    best = (bx0, by0, len(blob))
    return None if best is None else (best[0], best[1])


class Pointer(object):
    def __init__(self, host=VNC, verbose=True):
        self.c = api.connect(host)
        self.verbose = verbose
        self.vx, self.vy = 512, 384
        self.c.mouseMove(self.vx, self.vy)  # first event is only the baseline
        time.sleep(0.4)

    def log(self, msg):
        if self.verbose:
            print(msg)

    def shot(self, name=SHOT):
        self.c.captureScreen(name)
        time.sleep(0.25)
        return name

    def corner(self):
        """Slam into the top-left corner: the guest clamps, so we know where
        the pointer is even though the motion is relative."""
        self.c.mouseMove(1023, 767)
        time.sleep(0.3)
        for _ in range(3):
            self.c.mouseMove(0, 0)
            time.sleep(0.3)
        self.vx, self.vy = 0, 0

    def hop(self, x, y, step=25, dt=0.06):
        """Walk to a vnc point in small paced hops: big jumps lose motion,
        the ps/2 queue drops what it cannot keep up with."""
        x = max(0, min(1023, x))
        y = max(0, min(767, y))
        while (self.vx, self.vy) != (x, y):
            self.vx += max(-step, min(step, x - self.vx))
            self.vy += max(-step, min(step, y - self.vy))
            self.c.mouseMove(self.vx, self.vy)
            time.sleep(dt)

    def land(self, tx, ty, tol=2, tries=6, slam=True):
        """Put the arrow tip on the guest pixel (tx,ty)."""
        if slam:
            self.corner()
            self.hop(tx, ty, step=20, dt=0.08)
            time.sleep(0.8)
        g = None
        for _ in range(tries):
            self.shot(SHOT)
            g = find_arrow(SHOT, (self.vx - 70, self.vy - 70,
                                  self.vx + 30, self.vy + 30))
            if g is None:
                g = find_arrow(SHOT, (0, 0, 1024, 715))  # the dock is red too
            if g is None:
                self.log("  arrow not found near (%d,%d)" % (self.vx, self.vy))
                return None
            ex, ey = tx - g[0], ty - g[1]
            self.log("  arrow %s err (%d,%d)" % (g, ex, ey))
            if abs(ex) <= tol and abs(ey) <= tol:
                return g
            self.hop(self.vx + ex, self.vy + ey, step=12, dt=0.09)
            time.sleep(0.5)
        return g

    def click(self, btn=1):
        self.c.mouseDown(btn)
        time.sleep(0.08)
        self.c.mouseUp(btn)
        time.sleep(0.4)

    def double(self, btn=1):
        for i in range(2):
            self.c.mouseDown(btn)
            time.sleep(0.05)
            self.c.mouseUp(btn)
            if i == 0:
                time.sleep(0.12)
        time.sleep(0.8)

    def key(self, name, dt=0.15):
        self.c.keyPress(name)
        time.sleep(dt)

    def type(self, text, dt=0.07):
        """Type through the guest's Italian layout."""
        for ch in text:
            if ch == "-":
                name = "/"
            elif ch == "/":
                name = "shift-7"
            elif ch == ":":
                name = "shift-."
            else:
                name = ch
            self.c.keyPress(name)
            time.sleep(dt)

    def close(self):
        try:
            self.c.disconnect()
        except Exception:
            pass


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    p = Pointer()
    try:
        if cmd in ("click", "double", "move"):
            x, y = int(argv[2]), int(argv[3])
            p.land(x, y)
            if cmd == "click":
                p.click()
            elif cmd == "double":
                p.double()
        elif cmd == "shot":
            p.shot(argv[2] if len(argv) > 2 else SHOT)
        elif cmd == "type":
            p.type(" ".join(argv[2:]))
        else:
            print(__doc__)
            return 2
    finally:
        p.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
