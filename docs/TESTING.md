# Testing — methods and results (2026-10-21)

All results below were produced in this session; logs are in `build/`
(`unit_tests.log`, `play_tests.log`, `test_shots/*.log`) and emulator
screenshots in `build/evidence/` (they contain game art: keep private).

## How to run

```
make test                                  # unit tests + all gameplay scripts (headless, real data)
./build/host/host_runner DATA SCRIPT ...   # one script, see tests/host_runner.c for the script language
scripts/flycast_drive.sh FLYCAST build/oneshot.cdi tests/scripts/flycast_opening.txt OUTDIR   # real emulator
```

The host build compiles the *same* game code as the Dreamcast build (only the
platform file differs) with AddressSanitizer + UndefinedBehaviorSanitizer.

## Results

| Suite | Result |
|---|---|
| Toolchain proof (Milestone 1): `src/hello` → CDI → Flycast | **booted**, PVR output visible (screenshot `m1.png`) |
| Dreamcast build of the game (`make build`, GCC 15.2 `-O2`) | **OK**, 0 errors, 2 harmless `snprintf` truncation warnings |
| Disc image (`make cdi`, mkdcdisc 0.0.4) | **OK**, 148 MB (unpadded) |
| Flycast 2.5 boot (HLE BIOS, emulated VMU) | title → question → wake-up → "H-hello?" → walking → virtual computer → Documents → back to game: **all reached** |
| Flycast performance (opening room, diagnostics overlay) | 58–59 fps, 7.1–8.8 ms CPU/frame, heap ≈ 700 KB, textures ≈ 3 MB |
| `unit_tests` (VFS, save storage, persistence, bridge) | **85 passed, 0 failed** |
| `opening.txt` (boot → INIT handshake → Hello? → walk) | **PASS** |
| `safe_puzzle.txt` (original Map 20 safe, real data) | **PASS** (9 checks) |
| `restart_persistence.txt` (SAVE GAME → wallpaper → Close OneShot → relaunch → LOAD → name keyboard) | **PASS** |
| `no_vmu.txt` (no memory card) | **PASS** |
| `held_button.txt` (holding A cannot skip text) | **PASS** |
| Event commands used by the whole game vs. implemented | 55 / 55 handled (2 approximated) |
| Converted data validation (`tools/validate.py`) | **OK** — 260 assets, 65 maps, 128 MB; 1 warning (the original game's unused DEBUG map references a missing panorama) |

## Session 4 (2026-10-21): black ground in the Mining area (Map 21)

Root cause: map-sized panoramas larger than the PVR's 1024-px texture limit
(`mine` 1120×800 on Map 21, `maize`/`maize2` 1408×1280 on Map 31) were refused
by `plat_tex_create` on the Dreamcast, so the panorama - most of the visible
"ground" in those rooms - was never drawn; only areas covered by ordinary tiles
showed.  The host build has no size limit, so the scripted tests did not see it.

| Check | Result |
|---|---|
| Flycast, before fix (`build/oneshot_before_fix.cdi`), Map 21 at (53,20) | black except Niko (`render_flycast_01_BEFORE_…`) |
| Flycast, after fix, same spot + walking the quarry and back from Map 20 | panorama visible everywhere visited (`render_flycast_02/03_AFTER_…`) |
| Flycast, after fix, Map 31 (1408×1280 `maize`) | visible, 59 fps (`render_flycast_04_AFTER_…`) |
| `panorama.txt` (host) — Maps 21, 31, 35, 2 walked; no texture failures; 3-4 window refreshes in total | **PASS** |
| Frame rate, Flycast, diagnostics overlay | walking: 58-59 fps before and after; one-off room-entry stall while the room loads from disc: sampled 2 fps / 362 ms CPU before, 2 fps / 436 ms after (+~70 ms for the 512×512 window) |
| full suite (85 unit + 17 gameplay suites incl. movement, transitions, manual save) | **PASS** |

## Session 3 (2026-10-21): movement, collision, assets, manual saves

| Suite | Result |
|---|---|
| `movement.txt` — Map 3 basement stairs (30 diagonal touch events) down into Map 6 and back up through the top door to Map 5; jump in place / jump one tile; diagonal there-and-back | **PASS** (11 checks). Before the fix the player stuck at (10,8) forever |
| `door_sweep` (`make test-doors`, host) — all 125 touch / A-button events that contain a Teleport, entered from every standable side | 99 teleport; 17 start but don't teleport (in-event conditions, same-map teleports on Map 30, long conversations beyond the 7 s window); 6 debug/test-map teleports on unreachable tiles (to Map 7 "DEBUG"); Map 62 elevator doors need the ride to finish → covered by `elevator` |
| `elevator` (generated) — ride the Map 62 elevator (shake, two system message boxes, stop), then A at the door → Map 60 | **PASS** |
| `page_conditions.txt` — Map 34 "Calamus" page 4 needs Alula (actor 3) in the party | **PASS** (page 1 without her, page 4 with her) |
| `alula` (generated) — walk the 5×6 floor puzzle on the real panels using the game's own solution; check event sets switch 103 | **PASS** |
| `check_clue.py` — generated clue wallpaper vs. floor panels: same 5 columns × 6 rows, same lit cells, same orientation | **PASS** (0 mismatches) |
| `battle_anim.txt` — 5 animations loaded from `anim.dat`; the gatekeeper's balloon is drawn | **PASS** |
| `manual_save.txt` — Memory Card window: A saves (CE26) → "Game saved, 6 blocks per copy"; card full → refused, notice; card removed → refused, notice; relaunch loads the manual save (map, variable, item) | **PASS** (9 checks) |
| `manual_save_powercycle.txt` — new process, same card → INIT/LOAD restores the manual save | **PASS** |
| everything from sessions 1–2 + 85 unit tests | **PASS** |
| **Flycast 2.5**: manual save ×2 to the emulated VMU, then `tools/vmu_blocks.py` on Flycast's `vmu_save_A1.bin` | ONESHOT_A 6 blocks, ONESHOT_B 6 blocks = **12 blocks (6 144 bytes)**, 188/200 free; payload 2 188 bytes |
| **Flycast 2.5**: Debug Menu → Room 3 → hold Right → stairs → basement | **observed** (`build/evidence/movement_flycast_*.png`) |

Save size limits: typical payload ≈ 2.2 KB → 6 blocks per copy, 12 blocks
(6 KB) for both copies. Hard ceiling: the payload is capped at
`SAVE_MAX_PAYLOAD` = 12 KB → at most 26 blocks per copy, 52 blocks (26 KB)
for both — under the 90 KB limit; reaching it would need the virtual
Documents/Desktop persistent pool (6 KB) completely full, which the game never
does (it writes one ~7-byte document).

## Session 2 (2026-10-21): items, combinations, Debug Menu

| Suite | Result |
|---|---|
| `remote_window.txt` — pick up the TV remote, window *without* it (page 1, default description), select it in Items (CE20 runs, var 9 = 2, icon), window again → page 2 with the coloured password digits (vars 13–16), select again → put away; hold again → Y (Shift) puts it away | **PASS** (12 checks) |
| `combinations.txt` (generated locally from CE20 by `make test-gen`) — all 9 recipes through the Items menu, acid+sponge without/with rubber gloves, reversed selection order, invalid pair (CE32 "can't combine" runs, inventory unchanged) | **PASS** (81 checks) |
| `item_puzzles.txt` — alcohol+branch → TV → torch → fireplace lit (sw 11); lightbulb+empty battery → rowbot (sw 94); acid+sponge (gloves) → rowbot (sw 95); feather+ink → pen → gatekeeper (sw 114). Inventory/earlier-story switches injected ("SETUP" lines) | **PASS** (20 checks) |
| `debug_menu.txt` — hidden icon; L+R alone, partial, wrong order, other button in between, L+R only tapped → stays locked; correct sequence → unlocked, no window switch while held, repeat does nothing; Rooms scroll; R page cycling; music play/switch, B stops (game music restored), closing stops; room warp → map 5; the starting room's wake-up scene plays and returns to the Debug Menu with vars/items unchanged; ending cutscene (map 61) aborted with B → desktop, ending NOT recorded | **PASS** (36 checks) |
| all earlier scripts + 85 unit tests | **PASS** (no regressions) |
| Item obtainability (`tools/item_inventory.py`, static, whole game data) | every named item is added by at least one event or recipe; report in `build/generated/item_inventory.json` (local only: contains game text) |

Scripts and reports derived from your game data (`combinations`, `item_puzzles`,
`debug_menu`, item and debug indexes) are generated under `build/generated/` by
`make test-gen` and never committed; the committed tests check event pages,
switches, variables and items rather than quoting game text.
| **Flycast 2.5** `tests/scripts/flycast_items.txt` | remote found → Items menu → held icon → coloured digits: **observed** (`build/evidence/items_flycast_*.png`) |
| **Flycast 2.5** `tests/scripts/flycast_debug.txt` | hidden → L+R alone → unlocked → Rooms/scroll/Cutscenes/Music → play/stop → room warp → cutscene → back in Debug Menu: **observed** (`build/evidence/debug_flycast_*.png`) |

Two engine bugs were found and fixed on the way: autorun common events never
started (item use, notes, fast travel), and a button pressed in the menu or on
the virtual computer was seen by the map again when it resumed.

## Acceptance tests (spec §H) — where each is demonstrated

| # | Requirement | Evidence |
|---|---|---|
| 1 | modal dialog waits for controller | safe_puzzle (func_MessageBox Yes/No pauses the game until A/B) |
| 2 | event creates a virtual text file | safe_puzzle `expect-file /Documents/document.txt 1` |
| 3 | file visible in the file manager | safe_puzzle screenshots `safe_02_documents` (marked `*` new) |
| 4 | opening shows its contents | `safe_03_document_open`, `expect-opened` |
| 5 | puzzle not solvable before reading | blind input (000000) → `expect-sw 92 0`; code comes only from the file |
| 6 | correct answer advances the event | `expect-sw 92 1`, game shows "note added" |
| 7 | desktop change survives simulated restart | restart_persistence `expect-wallpaper clue` after relaunch |
| 8 | missing files / full storage / corrupt save / no VMU handled | unit tests (ENOENT, ENOSPC, CRC, truncation, no device) + no_vmu.txt |
| 9 | failed operation never reports success / advances | unit: torn write → `SAVE_VERIFY_FAILED`; full VFS → func_Document returns 0, no file |

## Stress tests (spec "Required stress tests")

| # | Scenario | Status |
|---|---|---|
| 1 | no VMU | tested (no_vmu.txt, unit) |
| 2 | insufficient free space | tested (unit `F_FULL`) |
| 3 | storage disappears | tested (unit `F_NO_DEVICE` after earlier saves) |
| 4 | interrupted save | tested (unit torn write; previous slot survives) |
| 5 | corrupted save | tested (bit flip, truncation; load → notice + new game, card untouched) |
| 6 | older valid save | partially: slot selection by sequence, unsupported version kept, integrity regeneration of the clue file |
| 7 | create / override / delete virtual files | tested (unit, incl. tombstones over disc files) |
| 8 | several restarts in succession | partially: 7 consecutive commits (unit); one relaunch per play script |
| 9 | revisit a room repeatedly | **not yet automated** |
| 10 | critical puzzle before and after saving | partially (integrity check after load) |
| 11 | wrong answers then retry | tested (safe: 000000 then the real code) |
| 12 | held button cannot skip | tested (held_button.txt; on-screen keyboard too) |
| 13 | disc stays read-only | by construction: `platform_dc.c` opens `/cd` with `O_RDONLY` only; all writes go to VFS RAM layers or the VMU |
| 14 | no puzzle needs disc writes | by construction (as 13) |

## Not tested

* **Real Dreamcast hardware** — not available in this session. Emulator
  success does not guarantee hardware behaviour (VMU timing, CD read speed,
  PAL 50 Hz, TV safe area on a CRT).
* Audio output (Flycast ran without an audio device; playback calls were exercised but not heard).
* Most of the game beyond the opening house and the safe (see PROGRESS.md).
