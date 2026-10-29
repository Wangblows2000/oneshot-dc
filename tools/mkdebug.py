#!/usr/bin/env python3
"""Build data/debug.dat for the hidden Debug Menu, discovered from the ORIGINAL data
(nothing is hand-listed):

  rooms     - every map in RPG_RT.lmt that has a map file.  Starting point = the
              destination of the Teleport commands that lead INTO the map from other
              maps (most frequently used one = the map's usual entrance); maps nobody
              teleports into use the party start (start map) or the map centre.
  cutscenes - every map event page with the Autorun trigger that shows a message or
              runs move routes (>= MIN_MOVES), i.e. a scripted scene.  Pages that run
              the "_init" host handshake (boot logic) are excluded.  The page's own
              start conditions are stored so the runtime can satisfy them.

Music is NOT listed here: the runtime lists the music assets of its own manifest.

Usage: mkdebug.py --game DIR --out DATA_DIR [--json REPORT]"""
import argparse, collections, glob, json, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import lcf
from convert import W

MIN_MOVES = 4
CMD_MESSAGE, CMD_MOVE, CMD_TELEPORT, CMD_SYSSFX = 10110, 11330, 10810, 10670


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--json")
    a = ap.parse_args()
    g = a.game
    tree, order, start = lcf.load_maptree(os.path.join(g, "RPG_RT.lmt"))
    db = lcf.load_database(os.path.join(g, "RPG_RT.ldb"))
    maps = {}
    for p in sorted(glob.glob(os.path.join(g, "Map*.lmu"))):
        mid = int(re.findall(r"(\d+)", os.path.basename(p))[0])
        try:
            maps[mid] = lcf.load_map(p)
        except Exception as e:  # a damaged map is reported, never fatal
            print(f"mkdebug: skipping {p}: {e}", file=sys.stderr)

    # ---- incoming teleports
    inc = collections.defaultdict(collections.Counter)
    def scan(cmds, src):
        for c in cmds:
            if c.code == CMD_TELEPORT and len(c.params) >= 3 and c.params[0] != src:
                inc[c.params[0]][(c.params[1], c.params[2], c.params[3] if len(c.params) > 3 else 0)] += 1
    for mid, m in maps.items():
        for ev in m.events:
            for pg in ev.pages:
                scan(pg.commands, mid)
    for ce in db.common_events.values():
        scan(ce["commands"], -1)

    rooms = []
    for mid in sorted(maps):
        info = tree.get(mid)
        name = info.name if info else f"MAP{mid:04d}"
        m = maps[mid]
        w, h = getattr(m, "width", 20), getattr(m, "height", 15)
        if inc[mid]:
            (x, y, d), _ = inc[mid].most_common(1)[0]
            src = 0
        elif mid == start["map"]:
            x, y, d, src = start["x"], start["y"], 0, 1
        else:
            x, y, d, src = w // 2, h // 2, 0, 2
        x = max(0, min(w - 1, x)); y = max(0, min(h - 1, y))
        rooms.append(dict(map=mid, name=name, x=x, y=y, dir=d, source=["teleport", "start", "centre"][src]))

    # ---- cutscenes
    cuts = []
    for mid in sorted(maps):
        mname = tree[mid].name if mid in tree else f"MAP{mid:04d}"
        for ev in maps[mid].events:
            for pg in ev.pages:
                if pg.trigger != 3:
                    continue
                codes = collections.Counter(c.code for c in pg.commands)
                if any(c.code == CMD_SYSSFX and c.string == "_init" for c in pg.commands):
                    continue
                if codes[CMD_MESSAGE] == 0 and codes[CMD_MOVE] < MIN_MOVES:
                    continue
                label = f"{mname}: {ev.name}" + (f" ({pg.index})" if pg.index > 1 else "")
                cuts.append(dict(map=mid, event=ev.id, page=pg.index, label=label[:60],
                                 flags=pg.cond_flags, sw_a=pg.switch_a, sw_b=pg.switch_b,
                                 var=pg.variable_id, val=pg.variable_value, op=pg.compare_op,
                                 item=pg.item_id, messages=codes[CMD_MESSAGE], moves=codes[CMD_MOVE]))

    w = W()
    w.raw(b"OSDB"); w.u16(1)
    w.u16(len(rooms))
    for r in rooms:
        w.u16(r["map"]); w.u16(r["x"]); w.u16(r["y"]); w.u8(r["dir"] & 0xFF)
        w.u8(["teleport", "start", "centre"].index(r["source"])); w.string(r["name"][:60])
    w.u16(len(cuts))
    for c in cuts:
        w.u16(c["map"]); w.u16(c["event"]); w.u8(c["page"]); w.u8(c["flags"] & 0xFF)
        w.u16(c["sw_a"]); w.u16(c["sw_b"]); w.u16(c["var"]); w.i32(c["val"]); w.u8(c["op"] & 0xFF)
        w.u16(c["item"]); w.string(c["label"])
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "debug.dat"), "wb") as f:
        f.write(bytes(w.b))
    print(f"mkdebug: {len(rooms)} rooms ({sum(r['source'] == 'teleport' for r in rooms)} with a teleport entrance), "
          f"{len(cuts)} cutscenes -> {os.path.join(a.out, 'debug.dat')}")
    if a.json:
        json.dump(dict(rooms=rooms, cutscenes=cuts), open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
