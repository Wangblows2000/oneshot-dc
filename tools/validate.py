#!/usr/bin/env python3
"""Validate converted data before it goes on a disc.

SPDX-License-Identifier: GPL-3.0-or-later

Checks: required files exist, headers/sizes are well-formed, every manifest
path exists, images decode, maps parse with in-range tile quarters, every
asset referenced by the converted data resolves, VFS base image is valid.
Exit code 1 on any error.  Usage: validate.py DATA_DIR
"""
import json
import os
import struct
import sys


def err(msgs, m):
    msgs.append(m)
    print("ERROR:", m)


def check_img(path, msgs):
    d = open(path, "rb").read()
    if d[:4] != b"OSI1":
        return err(msgs, f"{path}: bad magic")
    w, h = struct.unpack_from("<HH", d, 4)
    if not (0 < w <= 2048 and 0 < h <= 2048):
        return err(msgs, f"{path}: bad size {w}x{h}")
    if len(d) != 8 + 512 + w * h:
        return err(msgs, f"{path}: length {len(d)} != {8 + 512 + w * h}")


def check_wav(path, msgs, music):
    d = open(path, "rb").read(64)
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        return err(msgs, f"{path}: not RIFF/WAVE")
    fmt, ch, rate = struct.unpack_from("<HHI", d, 20)
    if music and (fmt, ch, rate) != (1, 2, 22050):
        err(msgs, f"{path}: music must be PCM stereo 22050 Hz, got fmt={fmt} ch={ch} rate={rate}")
    if not music and (fmt, ch) != (0x20, 1):
        err(msgs, f"{path}: sound must be mono Yamaha ADPCM (0x20), got fmt={fmt:#x} ch={ch}")


def main():
    data = sys.argv[1] if len(sys.argv) > 1 else "build/cdroot/data"
    msgs, warns = [], []
    for f in ("game.dat", "font.bin", "vfs_base.dat", "manifest.json"):
        if not os.path.isfile(os.path.join(data, f)):
            err(msgs, f"missing {f}")
    if msgs:
        sys.exit(1)
    mf = json.load(open(os.path.join(data, "manifest.json")))
    if os.path.getsize(os.path.join(data, "font.bin")) != 256 * 12:
        err(msgs, "font.bin wrong size")
    gd = open(os.path.join(data, "game.dat"), "rb").read()
    if gd[:4] != b"OSGD":
        err(msgs, "game.dat bad magic")
    vb = open(os.path.join(data, "vfs_base.dat"), "rb").read()
    if vb[:4] != b"OSVF":
        err(msgs, "vfs_base.dat bad magic")
    for a in mf["assets"]:
        p = os.path.join(data, a["path"])
        if not os.path.isfile(p):
            if a["cat"] in ("Sound", "Music") and not mf.get("audio", True):
                warns.append(f"audio not converted: {a['path']}")
                continue
            err(msgs, f"missing asset file {a['path']} ({a['cat']}/{a['name']})")
            continue
        if a["path"].endswith(".img"):
            check_img(p, msgs)
        elif a["path"].endswith(".wav"):
            check_wav(p, msgs, a["cat"] == "Music")
    for mid in mf["maps"]:
        p = os.path.join(data, "map", f"{mid:04d}.map")
        d = open(p, "rb").read()
        if d[:4] != b"OSMP":
            err(msgs, f"{p}: bad magic"); continue
        w, h = struct.unpack_from("<HH", d, 6)
        nl = d[14]                                   # u8 length of parallax name
        off = 15 + nl + 1 + 1 + 1 + 2 + 1 + 2 + w * h * 4
        q = d[off:off + w * h * 32]
        bad = sum(1 for i in range(0, len(q), 2) if q[i] != 0xFF and (q[i] >= 60 or q[i + 1] >= 32))
        if bad:
            err(msgs, f"map {mid}: {bad} tile quarters outside the 480x256 chipset")
    for m in mf.get("missing_refs", []):
        warns.append(f"original game references missing asset {m['cat']}/{m['name']} at {m['where']}")
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(data) for f in fs)
    print(f"validated {len(mf['assets'])} assets, {len(mf['maps'])} maps, {total / 1e6:.1f} MB")
    for w in warns:
        print("WARNING:", w)
    if total > 650e6:
        err(msgs, "data exceeds CD-R capacity budget (650 MB)")
    print("RESULT:", "FAIL" if msgs else "OK")
    sys.exit(1 if msgs else 0)


if __name__ == "__main__":
    main()
