# Progress log, TODO and known issues

## Session 1 — 2026-10-21

### Milestones

| Milestone | State |
|---|---|
| M0 Reconnaissance | done — docs/RECON.md |
| M1 Toolchain proof | done — GCC 15.2 sh-elf + KOS built from source; hello CDI boots in Flycast |
| M2 2D rendering | done — tiles (all autotile types), charsets, tile events, pictures, panoramas, window skin, text |
| M3 Playable room | done — movement, collision (RPG Maker passability), action button, touch events |
| M4 Events & dialogue | done — 55/55 used commands, choices, number input, switches/variables, branches, loops, labels, calls, parallel/autorun, teleports |
| M5 Original data pipeline | done — all 65 maps, 260 assets converted and validated |
| M6 Vertical slice | done — opening house + the Documents safe puzzle + save/relaunch/load |
| M7 Expand compatibility | started — see TODO |
| M8 Packaging & optimisation | partly — CDI built and booted; perf measured on emulator only |

### Files created
`src/` (runtime, ~6 000 lines of C incl. headers), `tools/` (converters, validator, reports),
`tests/` (unit tests, host runner, 5 gameplay scripts + 2 emulator scripts),
`scripts/` (env, setup, check, test runners, Flycast driver), `docs/`, `Makefile`, `Makefile.host`, `LICENSE`.

### Exact build commands used
```
scripts/setup_toolchain.sh   (equivalent steps were run manually: kos-chain make, KOS make, libwav make install, mkdcdisc meson)
make data GAME=assets/source_readonly/oneshot
make build && make cdi MKDCDISC=tools/bin/mkdcdisc
make test
scripts/flycast_drive.sh /opt/flycast/squashfs-root/AppRun build/oneshot.cdi tests/scripts/flycast_opening.txt OUT
```

## Session 4 — 2026-10-21

* **Fixed**: black ground in the Mining area (Map 21) and Maize (Map 31) on
  Dreamcast/Flycast — their map-sized panoramas exceed the 1024-px PVR texture
  limit and were silently dropped. Oversized images are now streamed through a
  512×512 window; non-scrolling panoramas are fitted to the map as in RPG Maker
  2003 (they used to scroll at half speed and drift away from the tiles).
* Diagnostics overlay shows free VRAM and failed texture loads.

## Session 3 — 2026-10-21

* **Movement** stays tile-based (RPG Maker 2003 and mkxp-oneshot both are);
  root cause of the stairs/doorway trouble: move-route diagonals (codes 4-7)
  and jumps (24-25) were not implemented → Map 3's stair events did nothing.
* **Page condition "actor in party"** was ignored (Map 34 Calamus page 4 was
  always active) → exported (map format 2) and checked.
* **ALULA**: the floor is 5 columns × 6 rows (switches 121-150 row by row); the
  generated clue wallpaper was drawn transposed (6 × 5). Now laid out from the
  panels' real positions. Puzzle data, movement and collision were correct.
* **Assets**: all 259 original files were already converted; the battle
  animation sheet `Battle/anim1` was never drawn → emote balloons implemented
  (`anim.dat`, `src/game/banim.c`).
* **VMU**: Memory Card window "A: Save now" runs the game's own save event;
  measured 6 blocks per copy (12 for both) on Flycast's emulated VMU.
* Dialog boxes: word-wrap, readable OK/Yes/No labels.

## Session 2 — 2026-10-21

* **Fixed**: autorun common events (trigger 3) were never started → items could
  not be used/combined, notes could not be read, fast travel did not open.
* **Fixed**: menu/desktop button presses leaked into the map when it resumed
  (selecting an item while facing something also "used" that thing).
* Items & combinations verified (all recipes in CE20, gloves condition, invalid
  pairs) and the puzzles that need combined items (torch/fireplace, rowbot, pen).
* Hidden Debug Menu (desktop): unlock sequence, Rooms / Cutscenes / Music.
* New tools: `tools/item_inventory.py`, `tools/mkdebug.py`; test generators in `tests/gen/`.

## TODO (in priority order)

1. **Real-hardware test** (CD-R + VMU): boot, controller, VMU save/load, CD
   streaming of music while reading maps, CRT safe area, PAL50.
2. Play through each area with scripted tests (Blue → Green → Red → Tower) and
   fix whatever each one exercises; add "revisit room" stress script.
3. Battle animations: drawn since session 3 ("target" flashes shown as screen flashes).
4. Picture effects (rotation/waver), PanScreen, transition styles other than fade/instant.
5. Footstep sounds by terrain (data has `terrain_data`; not used yet).
6. Default-menu details: item descriptions wrap, equipment shows only matching items.
7. Fast-travel menu (CE37) — now starts (autorun fix) but has no scripted test yet.
8. Ending sequences (Maps 59–66): `func_LeaveWindow`, `func_End`, close-enable — exercise end to end.
9. Texture memory: pack small pictures into an atlas if VRAM gets tight in picture-heavy scenes.
10. Optionally reverse-engineer the original message-box strings / clue image to replace the substitutes (only if it can be done lawfully from your own copy).

## Known bugs / limitations

* Message-box texts and the wallpaper clue art are substitutes (see COMPATIBILITY.md).
* Flycast was run without audio, so music/SFX playback is unverified by ear.
* Title screen "quit" opens the virtual computer (there is no OS to quit to).
* PAL consoles will run at 50 fps (game logic is frame-based like RPG Maker).
* The CDI for burning (`make cdi-burn`) is padded to ~700 MB; the emulator CDI is not padded.

## Next concrete task

Burn `build/oneshot-burn.cdi` (or use an ODE) and run it on a real Dreamcast with a
VMU: confirm boot, controls, the safe puzzle, and a save/power-off/load cycle.
Without hardware: script a full Blue-area playthrough (`tests/scripts/`) and fix the first failure.
