# Architecture

```
   your PC (offline)                                 Dreamcast (or headless Linux test build)
 ┌──────────────────────────┐   build/cdroot/data   ┌────────────────────────────────────────────┐
 │ tools/lcf.py  (LCF read) │──────────────────────▶│ resources/gamedata.c  load + bounds-check  │
 │ tools/convert.py         │  game.dat map/*.map   │ events/interp.c   RM2003 event interpreter │
 │ tools/autotile.py        │  img/*.img se/ bgm/   │ events/bridge.c   OneShot host functions   │
 │ tools/mkvfs.py           │  font.bin vfs_base    │ game/map.c        tiles, characters, moves │
 │ tools/validate.py        │                       │ game/transition.c restart state machine    │
 └──────────────────────────┘                       │ dialogue/ ui/ desktop/  windows & computer │
                                                    │ vfs/  save/  audio/  renderer/             │
                                                    │ platform/platform_dc.c  (KOS, PVR, VMU)    │
                                                    └────────────────────────────────────────────┘
```

## Modules

| Module | Responsibility |
|---|---|
| `platform/` | the only code that touches hardware: PVR drawing (2× scale, 640×480, TR list with autosort disabled = painter's order), maple controller, `/cd` files, `snd_sfx` + libwav, VMU (`vmufs`, `vmu_pkg`). `platform_host.c` implements the same API with a software rasterizer, scripted input and a directory "VMU" with fault injection |
| `resources/gamedata.c` | parses `game.dat`/`*.map`; every count, length and index is checked against `common.h` limits before use |
| `renderer/gfx.c` | indexed-image decode, bounded texture cache with **per-room lifetime** (purged on map change), CPU text canvases (uploaded only when changed), window skin |
| `game/map.c` | passability (RPG Maker rules incl. tile-event override and wall exceptions), event page selection, movement/animation timing, move routes, triggers, draw order (lower → events below → same layer (y-sorted) → "above" tiles → events above) |
| `events/interp.c` | main + parallel interpreters (48 map-event slots + one per common event), call stack, branches/loops/labels/choices, waits; unknown commands are logged and counted, never silently "succeed" |
| `events/bridge.c` | the 14 `func_*` operations + `_init` (see COMPATIBILITY.md) |
| `vfs/` | sandboxed virtual filesystem (below) |
| `save/` | two-slot CRC-checked save storage + TLV payload (`persist.c`) |
| `game/transition.c` | central state-transition manager (below) |
| `desktop/`, `ui/`, `dialogue/` | virtual computer, menus, on-screen keyboard, dialogs, message window |
| `desktop/debugmenu.c` | hidden Debug Menu back end: lists from `debug.dat` + the music manifest, room warp (`trans_teleport`), music preview (`audio_bgm_play`), sandboxed cutscene playback (below) |

## Big images (panoramas over 1024 px)

The PVR cannot hold a texture over 1024 px; `gfx.c` keeps such images indexed
in main RAM and streams only the on-screen part into one 512×512 window texture
(512 KB VRAM), refreshed when the camera leaves it. Panoramas follow RPG Maker
2003 placement: with "scroll" on they move at half speed (+ auto scroll),
otherwise they are fitted to the map (camera × (pano − screen) / (map − screen)).

## Movement model

Characters move on the tile grid, exactly like RPG Maker 2003 (and like
mkxp-oneshot's Game_Character, which also keeps a tile position plus a
sub-tile "real" position for smooth motion). A free, pixel-based movement
would not match how the original data was built: doors, stairs, the Alula
floor and every touch event are placed on tiles and fire on tile entry/bump.
Move-route commands follow EasyRPG Player: diagonal steps (4-7) try
vertical-then-horizontal, else horizontal-then-vertical, with RPG_RT's facing
rule; jumps (24-25) land on the summed offset and last one step with the
EasyRPG height curve. Page conditions include "actor in party" (0x10).

## Items (2014 mechanics, all in the original data)

Selecting an item in the Items menu turns on its "switch item" switch (21–51) →
the always-running common event **21 "item daemon"** copies the item id to
var 10 and turns on switch 119 → the autorun common event **20 "use ITEM"**
either *holds* the item (var 9 = id, icon picture 49), *drops* it (same item
again), or *combines* it with the held one (var 11/12 = the pair, smaller id
first; recipes and the "can't combine" event 32 are plain event code). Map
events react to the held item with page conditions / branches on var 9.
Autorun common events run when their switch is ON and nothing else is running
(map Autorun events first) — the same rule as RPG Maker 2003.

## Debug Menu (hidden)

Unlocked on the desktop's icon screen with **Up, Up, Down, Down, then L+R held
together for 0.5 s** (raw button edges, no auto-repeat; any other button resets;
works once per power-on, then the "Debug Menu" icon stays for the session).
Pages: Rooms (map tree + entrance from the data's teleports), Cutscenes (Autorun
pages with messages/move routes), Music (music assets). R cycles pages; L keeps
its normal "switch window" job.

Cutscene playback snapshots the game state (switches, variables, items, actors,
pictures, screen, message settings, position, music) and the bridge meta, turns
the **bridge sandbox** on (no VMU commit, no VFS write, no recorded ending, no
relaunch), satisfies the page's conditions, teleports to the map and starts that
page on the normal interpreter. When it finishes the snapshot is restored, the
map reloaded and the Debug Menu reopened; **B** stops it at any time and returns
to the desktop.

## Storage layers (read-only disc)

1. **Immutable disc** — `vfs_base.dat` + all assets; opened read-only; nothing writes to `/cd`.
2. **Session overlay (RAM)** — created/replaced/deleted virtual files for this power-on.
3. **Persistent overlay (RAM mirror of the save)** — files that must survive a restart
   (e.g. the safe-code document, the desktop clue file) and tombstones.

Lookup order: session → persistent → disc; the first hit (file, directory **or
tombstone**) wins, so a deleted disc file can never reappear. Paths must be
absolute, max 96 chars, depth ≤ 8, `[A-Za-z0-9 ._-()]`, no `.`/`..`/empty
components. Files ≤ 4 KB; pools: session 32 KB, persistent 6 KB, base 64 KB.
A failed write never destroys the previous contents.

**Save (VMU)**: `ONESHOT_A` / `ONESHOT_B`, each `OSSV` header (version, sequence,
length, CRC-32) + payload. A commit writes the slot that does *not* hold the
newest valid save, reads it back and checks the CRC before reporting success;
the other slot is never touched. Newer/unknown versions are reported and never
overwritten. A RAM copy of the last payload always exists, so simulated
restarts work without a VMU.

## Transition policy (game/transition.h)

| transition | room | session (switches, vars, items) | VFS session | VFS persistent | VMU |
|---|---|---|---|---|---|
| room change (Teleport) | replaced | kept | kept | kept | — |
| return to title | cleared | cleared | kept | kept | — |
| session restart | cleared | cleared | kept | kept | — |
| **relaunch** (OneShot closed & reopened) | cleared | cleared → boots Map 1, which loads the save itself | kept | kept | committed by the game's own save |
| load | cleared | from save | kept | from save | read |
| power cycle | cleared | cleared | **cleared** | from VMU | read |

Teleports during a running event keep the old map's command lists alive
("retired maps") until the main interpreter is idle, because RPG Maker keeps
executing the event after a Teleport.

## Memory and performance budget

* Main RAM 16 MB: code ~1 MB, game database ~0.3 MB, one map ≤ ~1 MB
  (largest map 104×74 cells × 40 bytes), CPU canvases < 0.5 MB.
  Measured heap in the opening room on Flycast: ~700 KB.
* VRAM 8 MB: textures padded to powers of two, ~3 MB in the opening room,
  purged on every map change; the cache is bounded at 96 textures.
* Sound RAM 2 MB: effects as 4-bit Yamaha ADPCM (4:1), unloaded per room;
  music streams from the disc (16-bit stereo 22.05 kHz, 88 KB/s).
* Flycast measurement: 58–59 fps, 7–9 ms CPU per frame (16.7 ms budget).
