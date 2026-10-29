#!/usr/bin/env python3
"""Report, from the original data, how every item is obtained, combined, used and removed.
Usage: item_inventory.py GAME_DIR [--json OUT]
The JSON contains names/text from YOUR copy of the game: write it under build/
(not committed), e.g. build/generated/item_inventory.json."""
import sys, os, json, glob, re
sys.path.insert(0, os.path.dirname(__file__))
import lcf

OPS = {0: '==', 1: '>=', 2: '<=', 3: '>', 4: '<', 5: '!='}

def walk(cmds):
    for c in cmds:
        yield c

def scan(g):
    db = lcf.load_database(g + '/RPG_RT.ldb')
    items = {k: v for k, v in db.item_info.items() if v['name']}
    gain, lose, held_use, page_cond, equip = {}, {}, {}, {}, {}
    def note(d, k, where): d.setdefault(k, []).append(where)
    def visit(cmds, where):
        for c in cmds:
            if c.code == 10320:  # ChangeItems: [op(0 add/1 remove), by var?, id, operand type, value]
                op, mode, iid = c.params[0], c.params[1], c.params[2]
                if mode == 0: note(gain if op == 0 else lose, iid, where)
            elif c.code == 12010 and c.params[0] == 1 and c.params[1] == 9 and c.params[2] == 0:
                note(held_use, c.params[3], where)          # "if var9 (held item) == N"
            elif c.code == 12010 and c.params[0] == 5 and c.params[2] == 5:
                note(equip, c.params[3], where)             # actor has item N equipped
    for k, ce in db.common_events.items():
        visit(ce['commands'], f"CE{k} {ce['name']}")
    for path in sorted(glob.glob(g + '/Map*.lmu')):
        mid = int(re.findall(r'(\d+)', os.path.basename(path))[0])
        try: m = lcf.load_map(path)
        except Exception: continue
        for ev in m.events:
            for p in ev.pages:
                w = f"Map{mid} ev{ev.id} '{ev.name}' p{p.index}"
                visit(p.commands, w)
                if p.cond_flags & 4 and p.variable_id == 9:
                    note(page_cond, p.variable_value, w)
    return db, items, gain, lose, held_use, page_cond, equip

def recipes(db):
    """Decode CE20 'use ITEM': nested 'var11 == a' / 'var12 == b' branches."""
    ce = db.common_events[20]['commands']
    out, a = [], None
    for i, c in enumerate(ce):
        if c.code == 12010 and c.params[:3] == [1, 11, 0]: a = c.params[3]
        if c.code == 12010 and c.params[:3] == [1, 12, 0] and a is not None:
            b = c.params[3]
            # collect the body until the matching else at the same indent
            body = []
            for d in ce[i + 1:]:
                if d.indent <= c.indent: break
                body.append(d)
            adds = [d.params[2] for d in body if d.code == 10320 and d.params[0] == 0]
            rems = [d.params[2] for d in body if d.code == 10320 and d.params[0] == 1]
            text = ' '.join(d.string for d in body if d.code in (10110, 20110))
            cond = [d.params for d in body if d.code == 12010 and d.params[0] == 5]
            out.append({'a': a, 'b': b, 'adds': adds, 'removes': rems, 'text': text, 'needs_equipped': [p[3] for p in cond]})
    return out

if __name__ == '__main__':
    g = sys.argv[1]
    db, items, gain, lose, held, pc, eq = scan(g)
    rec = recipes(db)
    rep = {'items': {}, 'recipes': rec}
    for k, v in sorted(items.items()):
        rep['items'][k] = {'name': v['name'], 'type': v['type'], 'obtained': gain.get(k, []),
                           'removed': lose.get(k, []), 'used_on (var9 page/branch)': pc.get(k, []) + held.get(k, []),
                           'equip_checks': eq.get(k, [])}
    if '--json' in sys.argv:
        os.makedirs(os.path.dirname(os.path.abspath(sys.argv[sys.argv.index('--json') + 1])), exist_ok=True)
        json.dump(rep, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)
    for k, r in rep['items'].items():
        src = 'COMBINE' if any(k in x['adds'] for x in rec) else ''
        print(f"{k:3} {r['name']:20} obtained:{len(r['obtained'])} {src:7} used:{len(r['used_on (var9 page/branch)'])} removed:{len(r['removed'])}")
    print('\nRECIPES (CE20, smaller id first):')
    for x in rec:
        n = lambda i: items.get(i, {}).get('name', i)
        print(f"  {n(x['a'])} + {n(x['b'])} -> +{[n(i) for i in x['adds']]} -{[n(i) for i in x['removes']]} equip{x['needs_equipped']} | {x['text'][:60]}")
