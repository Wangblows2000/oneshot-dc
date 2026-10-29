#!/usr/bin/env python3
"""Report which RPG Maker event commands the original data uses and which the
runtime implements (grep of src/events/interp.c case labels).

SPDX-License-Identifier: GPL-3.0-or-later
Usage: command_coverage.py GAME_DIR [--json OUT]
"""
import collections
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcf  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    game = sys.argv[1]
    names = {int(l.split(",")[0]): l.strip().split(",")[1] for l in open(os.path.join(HERE, "command_codes.csv"))}
    src = open(os.path.join(HERE, "..", "src", "events", "interp.c")).read()
    handled = {int(x) for x in re.findall(r"case (\d+):", src)}
    approx = {11060: "PanScreen (no-op)", 11210: "ShowBattleAnimation (wait only)"}
    use = collections.Counter()
    where = collections.defaultdict(set)
    db = lcf.load_database(os.path.join(game, "RPG_RT.ldb"))
    for i, ce in db.common_events.items():
        for c in ce["commands"]:
            use[c.code] += 1; where[c.code].add(f"CE{i}")
    for p in sorted(glob.glob(os.path.join(game, "Map*.lmu"))):
        mid = int(os.path.basename(p)[3:7])
        for ev in lcf.load_map(p).events:
            for pg in ev.pages:
                for c in pg.commands:
                    use[c.code] += 1; where[c.code].add(f"M{mid}")
    rows = []
    for code, n in sorted(use.items(), key=lambda x: -x[1]):
        st = "approximated" if code in approx else "implemented" if code in handled else "MISSING"
        rows.append(dict(code=code, name=names.get(code, "?"), uses=n, status=st, maps=len(where[code])))
        print(f"{code:6d} {names.get(code, '?'):26s} {n:5d}  {st}")
    missing = [r for r in rows if r["status"] == "MISSING"]
    print(f"\n{len(rows)} distinct commands used; {len(rows) - len(missing)} handled; {len(missing)} missing")
    if "--json" in sys.argv:
        json.dump(rows, open(sys.argv[sys.argv.index("--json") + 1], "w"), indent=1)


if __name__ == "__main__":
    main()
