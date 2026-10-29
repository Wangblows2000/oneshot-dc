"""Minimal read-only parser for RPG Maker 2000/2003 LCF data files.

Formats handled (verified against the actual OneShot files' headers):
  RPG_RT.ldb  -> header "LcfDataBase"
  RPG_RT.lmt  -> header "LcfMapTree"
  MapNNNN.lmu -> header "LcfMapUnit"

Chunk IDs come from EasyRPG liblcf's generator/csv/fields.csv (MIT licence);
no liblcf code is copied, only the documented field numbers are used.

Encoding: LCF stores everything as "chunks": BER-varint id, BER-varint size,
raw bytes.  A struct is a run of chunks terminated by id 0.  An Array<T> is
BER count followed by (BER index, struct) pairs.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

TEXT_ENCODING = "cp1252"  # verified heuristically: English game, no SJIS lead bytes in strings


class LcfError(Exception):
    pass


class Reader:
    def __init__(self, data: bytes, pos: int = 0, end: int | None = None):
        self.d = data
        self.p = pos
        self.end = len(data) if end is None else end

    def eof(self) -> bool:
        return self.p >= self.end

    def ber(self) -> int:
        v = 0
        for _ in range(6):
            if self.p >= self.end:
                raise LcfError(f"BER int runs past end at {self.p}")
            b = self.d[self.p]
            self.p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                # LCF stores negative ints as 32-bit two's complement in BER
                if v & 0x80000000:
                    v -= 1 << 32
                return v
        raise LcfError(f"BER int too long at {self.p}")

    def raw(self, n: int) -> bytes:
        if n < 0 or self.p + n > self.end:
            raise LcfError(f"read of {n} bytes past end at {self.p}")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def string(self) -> str:
        return self.raw(self.ber()).decode(TEXT_ENCODING, errors="replace")


def read_header(r: Reader, expect: str) -> None:
    h = r.raw(r.ber()).decode("ascii", errors="replace")
    if h != expect:
        raise LcfError(f"expected header {expect!r}, got {h!r}")


def read_chunks(r: Reader, until_zero: bool = True) -> dict[int, bytes]:
    """Read a struct: chunks until id 0 (or end of reader)."""
    out: dict[int, bytes] = {}
    while not r.eof():
        cid = r.ber()
        if cid == 0:
            break
        size = r.ber()
        out[cid] = r.raw(size)
    return out


def read_array(data: bytes) -> dict[int, dict[int, bytes]]:
    r = Reader(data)
    n = r.ber()
    out = {}
    for _ in range(n):
        idx = r.ber()
        out[idx] = read_chunks(r)
    return out


def c_int(ch: dict, cid: int, default: int = 0) -> int:
    return Reader(ch[cid]).ber() if cid in ch and ch[cid] else default


def c_str(ch: dict, cid: int, default: str = "") -> str:
    return ch[cid].decode(TEXT_ENCODING, errors="replace") if cid in ch else default


def c_i16vec(ch: dict, cid: int) -> list[int]:
    b = ch.get(cid, b"")
    return list(struct.unpack(f"<{len(b)//2}h", b[: len(b) // 2 * 2]))


def c_u8vec(ch: dict, cid: int) -> list[int]:
    return list(ch.get(cid, b""))


@dataclass
class Command:
    code: int
    indent: int
    string: str
    params: list[int]


def read_commands(data: bytes) -> list[Command]:
    """Event command list: no count; terminated by four zero bytes."""
    r = Reader(data)
    cmds: list[Command] = []
    while not r.eof():
        if r.d[r.p] == 0:
            break
        code = r.ber()
        indent = r.ber()
        s = r.string()
        n = r.ber()
        if n < 0 or n > 4096:
            raise LcfError(f"implausible param count {n}")
        params = [r.ber() for _ in range(n)]
        cmds.append(Command(code, indent, s, params))
    return cmds


@dataclass
class MoveCmd:
    code: int
    s: str = ""
    a: int = 0
    b: int = 0
    c: int = 0


@dataclass
class MoveRoute:
    cmds: list[MoveCmd]
    repeat: bool
    skippable: bool


def read_move_route(data: bytes) -> MoveRoute:
    ch = read_chunks(Reader(data))
    r = Reader(ch.get(0x0C, b""))
    cmds = []
    while not r.eof():
        code = r.ber()
        mc = MoveCmd(code)
        if code in (32, 33):
            mc.a = r.ber()
        elif code == 34:
            mc.s = r.string(); mc.a = r.ber()
        elif code == 35:
            mc.s = r.string(); mc.a = r.ber(); mc.b = r.ber(); mc.c = r.ber()
        cmds.append(mc)
    return MoveRoute(cmds, bool(c_int(ch, 0x15, 1)), bool(c_int(ch, 0x16, 0)))


@dataclass
class Page:
    index: int
    cond_flags: int
    switch_a: int
    switch_b: int
    variable_id: int
    variable_value: int
    item_id: int
    charset: str
    char_index: int
    direction: int
    pattern: int
    translucent: bool
    move_type: int
    move_freq: int
    trigger: int
    layer: int
    overlap_forbidden: bool
    anim_type: int
    move_speed: int
    commands: list[Command]
    compare_op: int = 1
    actor_id: int = 1
    route: "MoveRoute | None" = None


@dataclass
class Event:
    id: int
    name: str
    x: int
    y: int
    pages: list[Page] = field(default_factory=list)


def parse_page(idx: int, ch: dict) -> Page:
    cond = read_chunks(Reader(ch.get(0x02, b"")))
    return Page(
        index=idx,
        cond_flags=(cond.get(0x01, b"\0") or b"\0")[0],
        switch_a=c_int(cond, 0x02, 1),
        switch_b=c_int(cond, 0x03, 1),
        variable_id=c_int(cond, 0x04, 1),
        variable_value=c_int(cond, 0x05, 0),
        item_id=c_int(cond, 0x06, 1),
        actor_id=c_int(cond, 0x07, 1),
        compare_op=c_int(cond, 0x0A, 1),
        charset=c_str(ch, 0x15),
        char_index=c_int(ch, 0x16, 0),
        direction=c_int(ch, 0x17, 2),
        pattern=c_int(ch, 0x18, 1),
        translucent=bool(c_int(ch, 0x19, 0)),
        move_type=c_int(ch, 0x1F, 1),
        move_freq=c_int(ch, 0x20, 3),
        trigger=c_int(ch, 0x21, 0),
        layer=c_int(ch, 0x22, 0),
        overlap_forbidden=bool(c_int(ch, 0x23, 0)),
        anim_type=c_int(ch, 0x24, 0),
        move_speed=c_int(ch, 0x25, 3),
        commands=read_commands(ch.get(0x34, b"")),
        route=read_move_route(ch.get(0x29, b"")),
    )


@dataclass
class MapUnit:
    chipset_id: int
    width: int
    height: int
    scroll_type: int
    parallax_flag: bool
    parallax_name: str
    lower: list[int]
    upper: list[int]
    events: list[Event]
    parallax_loop_x: bool = False
    parallax_loop_y: bool = False
    parallax_auto_x: bool = False
    parallax_sx: int = 0
    parallax_auto_y: bool = False
    parallax_sy: int = 0


def load_map(path: str) -> MapUnit:
    d = open(path, "rb").read()
    r = Reader(d)
    read_header(r, "LcfMapUnit")
    ch = read_chunks(r)
    w, h = c_int(ch, 0x02, 20), c_int(ch, 0x03, 15)
    lower, upper = c_i16vec(ch, 0x47), c_i16vec(ch, 0x48)
    if len(lower) != w * h or len(upper) != w * h:
        raise LcfError(f"{path}: layer size mismatch {len(lower)},{len(upper)} vs {w}x{h}")
    events = []
    for eid, ech in read_array(ch.get(0x51, b"\0")).items():
        ev = Event(eid, c_str(ech, 0x01), c_int(ech, 0x02), c_int(ech, 0x03))
        for pid, pch in read_array(ech.get(0x05, b"\0")).items():
            ev.pages.append(parse_page(pid, pch))
        events.append(ev)
    return MapUnit(
        chipset_id=c_int(ch, 0x01, 1), width=w, height=h,
        scroll_type=c_int(ch, 0x0B, 0),
        parallax_flag=bool(c_int(ch, 0x1F, 0)), parallax_name=c_str(ch, 0x20),
        lower=lower, upper=upper, events=events,
        parallax_loop_x=bool(c_int(ch, 0x21)), parallax_loop_y=bool(c_int(ch, 0x22)),
        parallax_auto_x=bool(c_int(ch, 0x23)), parallax_sx=c_int(ch, 0x24),
        parallax_auto_y=bool(c_int(ch, 0x25)), parallax_sy=c_int(ch, 0x26),
    )


@dataclass
class Database:
    switches: dict[int, str]
    variables: dict[int, str]
    chipsets: dict[int, dict]
    common_events: dict[int, dict]
    items: dict[int, str]
    actors: dict[int, dict]
    system: dict
    terms: dict[int, str]


def load_database(path: str) -> Database:
    d = open(path, "rb").read()
    r = Reader(d)
    read_header(r, "LcfDataBase")
    ch = read_chunks(r)
    sw = {i: c_str(c, 1) for i, c in read_array(ch.get(0x17, b"\0")).items()}
    va = {i: c_str(c, 1) for i, c in read_array(ch.get(0x18, b"\0")).items()}
    chips = {}
    for i, c in read_array(ch.get(0x14, b"\0")).items():
        chips[i] = dict(
            name=c_str(c, 1), file=c_str(c, 2),
            terrain=c_i16vec(c, 3),
            pass_lower=c_u8vec(c, 4) or [15] * 162,
            pass_upper=c_u8vec(c, 5) or ([31] + [15] * 143),
            anim_type=c_int(c, 0x0B, 0), anim_speed=c_int(c, 0x0C, 0),
        )
    ces = {}
    for i, c in read_array(ch.get(0x19, b"\0")).items():
        ces[i] = dict(name=c_str(c, 1), trigger=c_int(c, 0x0B, 5),
                      switch_flag=bool(c_int(c, 0x0C, 0)), switch_id=c_int(c, 0x0D, 1),
                      commands=read_commands(c.get(0x16, b"")))
    items = {i: c_str(c, 1) for i, c in read_array(ch.get(0x0D, b"\0")).items()}
    item_info = {i: dict(name=c_str(c, 1), desc=c_str(c, 2), type=c_int(c, 3), switch_id=c_int(c, 0x37, 1))
                 for i, c in read_array(ch.get(0x0D, b"\0")).items()}
    skills = {i: dict(name=c_str(c, 1), desc=c_str(c, 2), type=c_int(c, 8), switch_id=c_int(c, 0x0D, 1))
              for i, c in read_array(ch.get(0x0C, b"\0")).items()}
    actors = {i: dict(name=c_str(c, 1), charset=c_str(c, 3), char_index=c_int(c, 4),
                      face=c_str(c, 0x0F), face_index=c_int(c, 0x10))
              for i, c in read_array(ch.get(0x0B, b"\0")).items()}
    sysch = read_chunks(Reader(ch.get(0x16, b"")))
    system = dict(
        ldb_id=c_int(sysch, 0x0A), title=c_str(sysch, 0x11), gameover=c_str(sysch, 0x12),
        system=c_str(sysch, 0x13), system2=c_str(sysch, 0x14),
        party=c_i16vec(sysch, 0x16), font_id=c_int(sysch, 0x48),
        show_title=bool(c_int(sysch, 0x6F, 1)), has_2k3_fields=any(k >= 0x14 for k in [0x14] if 0x14 in sysch),
        title_music=read_chunks(Reader(sysch.get(0x1F, b""))),
        sounds={k: read_chunks(Reader(sysch.get(cid, b""))) for k, cid in
                (("cursor", 0x29), ("decision", 0x2A), ("cancel", 0x2B), ("buzzer", 0x2C))},
        transition_out=c_int(sysch, 0x3D), transition_in=c_int(sysch, 0x3E),
        message_stretch=c_int(sysch, 0x47),
    )
    tch = read_chunks(Reader(ch.get(0x15, b"")))
    terms = {k: c_str(tch, k) for k in (0x6A, 0x6B, 0x6C, 0x70, 0x72, 0x73, 0x75, 0x97, 0x98, 0x99)}
    system["menu_commands"] = c_i16vec(sysch, 0x1B)
    db = Database(sw, va, chips, ces, items, actors, system, terms)
    db.item_info = item_info  # type: ignore[attr-defined]
    db.skills = skills  # type: ignore[attr-defined]
    db.raw_top_chunks = sorted(ch.keys())  # type: ignore[attr-defined]
    db.raw_chunks = ch  # type: ignore[attr-defined]
    return db


@dataclass
class MapInfo:
    id: int
    name: str
    parent: int
    type: int
    music_type: int
    music: str


def load_maptree(path: str):
    d = open(path, "rb").read()
    r = Reader(d)
    read_header(r, "LcfMapTree")
    n = r.ber()
    maps = {}
    for _ in range(n):
        idx = r.ber()
        c = read_chunks(r)
        mus = read_chunks(Reader(c.get(0x0C, b"")))
        maps[idx] = MapInfo(idx, c_str(c, 1), c_int(c, 2), c_int(c, 4, -1), c_int(c, 0x0B),
                            c_str(mus, 1))
    order_n = r.ber()
    order = [r.ber() for _ in range(order_n)]
    active = r.ber()
    start = read_chunks(r)
    return maps, order, dict(map=c_int(start, 1), x=c_int(start, 2), y=c_int(start, 3))
