"""Dev helper: print an event's pages/commands.  Usage: dump_event.py GAME_DIR MAP EVENT|CE N"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import lcf
names = {int(l.split(',')[0]): l.strip().split(',')[1] for l in open(os.path.join(os.path.dirname(__file__), 'command_codes.csv'))}
def dump(cmds):
    for c in cmds:
        print('   ', '  ' * c.indent, names.get(c.code, c.code), repr(c.string) if c.string else '', c.params)
g = sys.argv[1]
if sys.argv[2] == 'CE':
    ce = lcf.load_database(g + '/RPG_RT.ldb').common_events[int(sys.argv[3])]
    print(ce['name'], 'trigger', ce['trigger']); dump(ce['commands'])
else:
    m = lcf.load_map(f"{g}/Map{int(sys.argv[2]):04d}.lmu")
    for ev in m.events:
        if ev.id == int(sys.argv[3]):
            print(f'Event {ev.id} {ev.name!r} at {ev.x},{ev.y}')
            for p in ev.pages:
                print(f' PAGE {p.index} cond_flags={p.cond_flags} swA={p.switch_a} swB={p.switch_b} var={p.variable_id}>={p.variable_value} item={p.item_id} trigger={p.trigger} gfx={p.charset}#{p.char_index} layer={p.layer}')
                dump(p.commands)
