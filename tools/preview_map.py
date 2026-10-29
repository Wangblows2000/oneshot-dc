#!/usr/bin/env python3
"""Render a converted map (map/NNNN.map + chipset .img) to PNG on the host.

SPDX-License-Identifier: GPL-3.0-or-later

This reads ONLY converted data, so it verifies the converter output that the
Dreamcast runtime consumes (tile quarters, chipset image, palette).
Usage: preview_map.py DATA_DIR MAP_ID OUT.png [frame]
"""
import os
import struct
import sys

from PIL import Image


def read_str(d, p):
    n = d[p]
    return d[p + 1:p + 1 + n].decode("cp1252"), p + 1 + n


def load_img(path, key0=True):
    d = open(path, "rb").read()
    assert d[:4] == b"OSI1"
    w, h = struct.unpack_from("<HH", d, 4)
    pal = struct.unpack_from("<256H", d, 8)
    px = d[8 + 512:8 + 512 + w * h]
    im = Image.new("RGBA", (w, h))
    cols = []
    for c in pal:
        r, g, b = (c >> 10) & 31, (c >> 5) & 31, c & 31
        cols.append((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255))
    if key0:
        cols[0] = (0, 0, 0, 0)
    im.putdata([cols[i] for i in px])
    return im


def load_manifest(data):
    d = open(os.path.join(data, "game.dat"), "rb").read()
    import json
    mf = json.load(open(os.path.join(data, "manifest.json")))
    return {(a["cat"], a["name"].lower()): a["path"] for a in mf["assets"]}


def chipset_file(data, chip_id):
    # parse game.dat just far enough to find chipset names
    d = open(os.path.join(data, "game.dat"), "rb").read()
    p = 6
    p += 4  # n_switches, n_vars
    for _ in range(4):
        _, p = read_str(d, p)
    for _ in range(4):
        _, p = read_str(d, p); p += 3
    _, p = read_str(d, p); p += 3
    n = struct.unpack_from("<H", d, p)[0]; p += 2
    for _ in range(n):
        _, p = read_str(d, p); _, p = read_str(d, p); p += 1; _, p = read_str(d, p); p += 1
    n = struct.unpack_from("<H", d, p)[0]; p += 2
    for _ in range(n):
        _, p = read_str(d, p)
    n = struct.unpack_from("<H", d, p)[0]; p += 2
    for i in range(1, n + 1):
        name, p = read_str(d, p)
        if i == chip_id:
            return name
        p += 2 + 162 + 144
    raise KeyError(chip_id)


def main():
    data, mid, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    frame = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    d = open(os.path.join(data, "map", f"{mid:04d}.map"), "rb").read()
    assert d[:4] == b"OSMP"
    w, h, chip, scroll, pflag = struct.unpack_from("<HHHBB", d, 6)
    p = 6 + 8
    _, p = read_str(d, p)
    p += 1 + 1 + 1 + 2 + 1 + 2
    p += w * h * 4  # raw lower+upper ids
    lowerq = d[p:p + w * h * 4 * 4 * 2]; p += w * h * 4 * 4 * 2
    upperq = d[p:p + w * h * 4 * 2]
    man = load_manifest(data)
    cs = load_img(os.path.join(data, man[("ChipSet", chipset_file(data, chip).lower())]))
    lower_cs = load_img(os.path.join(data, man[("ChipSet", chipset_file(data, chip).lower())]), key0=False)
    im = Image.new("RGBA", (w * 16, h * 16), (0, 0, 0, 255))
    for cy in range(h):
        for cx in range(w):
            base = ((cy * w + cx) * 4 + frame) * 8
            for q in range(4):
                sx, sy = lowerq[base + q * 2], lowerq[base + q * 2 + 1]
                if sx == 0xFF:
                    continue
                tile = cs.crop((sx * 8, sy * 8, sx * 8 + 8, sy * 8 + 8))
                im.alpha_composite(tile, (cx * 16 + (q % 2) * 8, cy * 16 + (q // 2) * 8))
            base = (cy * w + cx) * 8
            for q in range(4):
                sx, sy = upperq[base + q * 2], upperq[base + q * 2 + 1]
                if sx == 0xFF:
                    continue
                tile = cs.crop((sx * 8, sy * 8, sx * 8 + 8, sy * 8 + 8))
                im.alpha_composite(tile, (cx * 16 + (q % 2) * 8, cy * 16 + (q // 2) * 8))
    im.save(out)
    print(f"map {mid}: {w}x{h} chipset {chip} -> {out}")


if __name__ == "__main__":
    main()
