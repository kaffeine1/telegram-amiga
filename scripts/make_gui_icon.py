#!/usr/bin/env python3
"""Turn a classic Workbench icon into the self-launching TelegramAmiga icon.

Both launcher icons of a package are the SAME bytes: TelegramAmiga.info owns
the binary, TelegramAmiga-TUI.info owns a 0-byte marker whose name carries
"TUI", and the binary tells the two apart by the name it was started with.
For that to work every icon we ship must be a PROJECT icon (Workbench then
runs its DefaultTool with the file as the argument), its DefaultTool must be
TelegramAmiga, and its stack 1048576 (the first login does a DH exchange that
Gurus at 262144). This script imposes those three fields on any classic icon,
including one drawn by somebody else, and touches nothing else: the imagery,
the tool types and a trailing OS3.5/OS4 colour icon (FORM ICON) are kept
byte for byte.

The .info layout it walks (the icon.library one, big-endian):
  [0:78]    DiskObject: magic e310, version, the Gadget (imagery pointers and
            UserData), do_Type at 48, then the DefaultTool/ToolTypes/DrawerData/
            ToolWindow pointers used as "present" flags, StackSize at 74
  DrawerData      56 bytes, only when the pointer is set
  Image + planes  for GadgetRender, then for SelectRender, when set;
                  a plane row is ((width+15)/16)*2 bytes, depth planes
  DefaultTool     LONG length (counting the NUL) + chars, when set
  ToolTypes       LONG (n+1)*4, then n x (LONG length + chars), when set
  ToolWindow      LONG length + chars, when set
  DrawerData2     6 bytes, only with DrawerData and Gadget.UserData == 1
  trailing        whatever follows (FORM ICON, NewIcons chunks), kept as is

A round trip with no edits must reproduce the input exactly; the script
refuses an icon it cannot reproduce rather than guess.

Usage:
  make_gui_icon.py IN.info OUT.info [tool] [stack] [--project] [--drop-tooltypes]
  make_gui_icon.py --dump IN.info
"""
import struct
import sys

WBPROJECT = 4


def _image_size(data, p):
    w, h, depth = struct.unpack(">hhh", data[p + 4:p + 10])
    return 20 + ((w + 15) // 16) * 2 * h * depth


def parse(data):
    if data[0:2] != b"\xe3\x10":
        raise ValueError("not a .info file (bad magic)")
    if len(data) < 78:
        raise ValueError("truncated DiskObject")
    render, select = struct.unpack(">II", data[22:30])
    userdata = struct.unpack(">I", data[44:48])[0]
    do_type = data[48]
    dtool, ttypes, drawer, toolwin, stack = struct.unpack(
        ">IIxxxxxxxxIII", data[50:78])
    p = 78
    s = {"header": bytearray(data[0:78]), "do_type": do_type, "stack": stack,
         "has_dtool": dtool != 0}
    s["drawer"] = data[p:p + 56] if drawer else b""
    p += len(s["drawer"])
    imagery = p
    if render:
        p += _image_size(data, p)
    if select:
        p += _image_size(data, p)
    s["imagery"] = data[imagery:p]
    s["dt_str"] = b""
    if dtool:
        n = struct.unpack(">I", data[p:p + 4])[0]
        s["dt_str"] = data[p + 4:p + 4 + n]
        p += 4 + n
    tt = p
    if ttypes:
        cnt = struct.unpack(">I", data[p:p + 4])[0]
        p += 4
        for _ in range(cnt // 4 - 1):
            n = struct.unpack(">I", data[p:p + 4])[0]
            p += 4 + n
    s["tooltypes"] = data[tt:p]
    s["toolwin"] = b""
    if toolwin:
        n = struct.unpack(">I", data[p:p + 4])[0]
        s["toolwin"] = data[p:p + 4 + n]
        p += 4 + n
    s["drawer2"] = b""
    if drawer and userdata == 1:
        s["drawer2"] = data[p:p + 6]
        p += 6
    if p > len(data):
        raise ValueError("icon ends inside a record")
    s["trailing"] = data[p:]
    return s


def serialize(s, default_tool=None, stack=None, project=False,
              drop_tooltypes=False):
    header = bytearray(s["header"])
    if project:
        header[48] = WBPROJECT
    if stack is not None:
        header[74:78] = struct.pack(">I", stack)
    dt = s["dt_str"]
    if default_tool is not None:
        dt = default_tool.encode("latin-1") + b"\x00"
        if not s["has_dtool"]:
            header[50:54] = struct.pack(">I", 1)  # any non-zero: "present"
    tooltypes = s["tooltypes"]
    if drop_tooltypes:
        header[54:58] = struct.pack(">I", 0)
        tooltypes = b""
    out = bytes(header) + s["drawer"] + s["imagery"]
    if dt:
        out += struct.pack(">I", len(dt)) + dt
    out += tooltypes + s["toolwin"] + s["drawer2"] + s["trailing"]
    return out


def describe(s):
    tt = []
    p = 0
    if s["tooltypes"]:
        cnt = struct.unpack(">I", s["tooltypes"][0:4])[0]
        p = 4
        for _ in range(cnt // 4 - 1):
            n = struct.unpack(">I", s["tooltypes"][p:p + 4])[0]
            tt.append(s["tooltypes"][p + 4:p + 4 + n].rstrip(b"\x00"))
            p += 4 + n
    w, h = struct.unpack(">hh", s["header"][12:16])
    kind = "trailing %d bytes" % len(s["trailing"])
    if s["trailing"][:4] == b"FORM":
        kind += " (%s)" % s["trailing"][8:12].decode("latin-1")
    return ("do_Type=%d gadget %dx%d imagery %d bytes DefaultTool=%r stack=%d "
            "tooltypes=%r drawerdata=%s %s"
            % (s["do_type"], w, h, len(s["imagery"]),
               s["dt_str"].rstrip(b"\x00").decode("latin-1"), s["stack"], tt,
               "yes" if s["drawer"] else "no", kind))


def main(argv):
    flags = [a for a in argv[1:] if a.startswith("--")]
    args = [a for a in argv[1:] if not a.startswith("--")]
    if "--dump" in flags and len(args) == 1:
        try:
            s = parse(open(args[0], "rb").read())
        except (ValueError, struct.error) as e:
            print("%s: not a classic icon I can read (%s)" % (args[0], e))
            return 1
        print("%s: %s" % (args[0], describe(s)))
        return 0
    if len(args) < 2:
        sys.stderr.write(__doc__)
        return 2
    src, dst = args[0], args[1]
    tool = args[2] if len(args) > 2 else "TelegramAmiga"
    stack = int(args[3]) if len(args) > 3 else 1048576
    data = open(src, "rb").read()
    try:
        s = parse(data)
    except (ValueError, struct.error) as e:
        sys.stderr.write("FATAL: %s: not a classic icon I can read (%s)\n"
                         % (src, e))
        return 1
    rt = serialize(s)
    if rt != data:
        sys.stderr.write("FATAL: round-trip mismatch (%d vs %d bytes): the "
                         "parser does not understand this icon; refusing.\n"
                         % (len(rt), len(data)))
        return 1
    out = serialize(s, default_tool=tool, stack=stack,
                    project="--project" in flags,
                    drop_tooltypes="--drop-tooltypes" in flags)
    # What we wrote must read back as what we meant.
    back = parse(out)
    if (back["dt_str"] != tool.encode("latin-1") + b"\x00" or
            back["stack"] != stack or
            ("--project" in flags and back["do_type"] != WBPROJECT) or
            back["trailing"] != s["trailing"] or back["imagery"] != s["imagery"]):
        sys.stderr.write("FATAL: the rewritten icon does not read back as "
                         "intended; refusing.\n")
        return 1
    open(dst, "wb").write(out)
    print("wrote %s (%d bytes): %s" % (dst, len(out), describe(back)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
