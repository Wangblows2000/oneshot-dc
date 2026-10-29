# Compatibility registry — OneShot host operations on Dreamcast

Machine-readable versions (regenerate with the commands at the end):
`docs/generated/compatibility_inventory.json` (every call site + tests),
`docs/generated/bridge_registry.json` (from the C table in `src/events/bridge.c`),
`docs/generated/command_coverage.json` (event commands used vs. implemented).

**Verified** = read from the game data. **Inferred** = deduced from how the
events use a result (the native code is inside an obfuscated EXE and was not
reverse engineered). **Substitute** = Dreamcast text/art standing in for
something that only exists inside that EXE.

| # | Operation (CE) | Where the data calls it | In → Out | Dreamcast replacement | Persistence / restart | Status | Tests |
|---|---|---|---|---|---|---|---|
| 0 | func_GuessName (CE1) | Map16 ev7 (the Prophet) | — → actor 2 name | copies the virtual computer's account name (Settings app) | none | implemented (approximation: no OS user name exists) | manual |
| 1 | func_SetName (CE2) | 7 sites before `_PlayerName_…` messages | — | placeholder always replaced by the stored player name | — | implemented | safe_puzzle |
| 2 | func_SetNameEntry (CE3) | Map16 ev7 after EnterHeroName | actor 2 name → stored name | stored in persistent meta (saved with next commit) | survives power cycle once saved | implemented | restart_persistence |
| 3 | func_ShakeWindow (CE4) | Map60 ev1 (non-photosensitive path) | — | 1 s screen shake | — | approximation | — |
| 4 | func_SetWallpaper (CE5) | Map33 ev1 (arg 1 / arg 0) | arg1 → return 1 ok | virtual desktop wallpaper; clue art generated from Map28 ev36's solution | meta, saved | implemented; **clue art reconstructed** | restart_persistence, unit |
| 5 | func_MessageBox (CE6) | Map2 ev7 (0), Map20 ev3 (1,2,3), Map61/62 (4,5,7) | arg1 → return 1 Yes/OK, 0 No | modal dialog, controller-confirmed, game paused | — | implemented; **texts are substitutes** | safe_puzzle |
| 6 | func_LeaveWindow (CE7) | Map61 ev1 (ending) | — | window-leaves animation → "OneShot has closed" → relaunch | relaunch policy | approximation | — |
| 7 | func_Save (CE8) | CE26 SAVE GAME | — | snapshot switches/vars | — | implemented | unit, restart_persistence |
| 8 | func_WriteItem (CE9) | CE26 (43 calls) | arg1 id → — | items until first 0, then de-duplicated note set (CE26 re-sends stale args), commit at end of frame | VMU two-slot commit; RAM copy | implemented | unit |
| 9 | func_Load (CE10) | CE27 LOAD GAME (Map1 INIT) | → 0 none / 1 ok / 2 name unknown | newest valid slot, else RAM copy; integrity check regenerates a missing clue document | — | implemented | unit, restart_persistence, no_vmu |
| 10 | func_ReadItem (CE11) | CE27 loops | → next id / 0 | streams restored lists | — | implemented | unit |
| 11 | func_Document (CE12) | Map20 ev3 pages 1–2 (safe) | var29 → file | writes `/Documents/document.txt` (persistent overlay), marks it new on the desktop | survives relaunch; power cycle once saved | implemented | safe_puzzle, unit |
| 12 | func_End (CE13) | Map60 ev1 (arg 4 / 3) | arg1 → ENDING | ENDING = arg1−1 (matches Map1's `var7==2` / `>2` checks), committed immediately | power cycle (if VMU) | implemented; mapping inferred | unit |
| 13 | func_SetCloseEnabled (CE14) | Map60/61 | arg1 | enables "Close OneShot" in the virtual computer | — | implemented | — |
| — | `_init` handshake | Map1 ev3 | signature → cleared | clears switches 1–20 / vars 1–4, sets var7 from meta | — | implemented (inferred) | opening |

## Known deviations from the Windows original

1. **Message box wording** (#5) and the **clue wallpaper picture** (#4) are not
   the original strings/image (they live inside the obfuscated EXE). The
   wallpaper is drawn from the solution the game itself checks; the message
   boxes ask Yes/No questions whose answers lead to the same branches.
2. **Name guessing** (#0) cannot read a real account name; it uses the
   virtual computer's account name instead.
3. **"Deja vu" (ENDING = 1)** — the data shows a value 1 exists; when the
   original set it is unknown. Inferred rule: launched before, no save, no ending.
4. **Closing the game** is simulated (no process to exit on a Dreamcast):
   "Close OneShot" in the computer → relaunch through Map 1. A real power-off
   loses anything that was never committed to a VMU; the game says so.
5. **Debug Menu** (Dreamcast-only addition, hidden): see ARCHITECTURE.md. Its
   cutscene player forces the chosen page even if RPG Maker would pick a
   different page of that event with the current switches.
6. **Battle animations** (ShowBattleAnimation, 30 uses: the emote balloons
   Question / Exclamation / Love / Sweat / Frustration) are drawn from
   `Battle/anim1` via `anim.dat`, timed and placed like EasyRPG Player; a
   "target" flash is shown as a screen flash. **PanScreen** (4 uses) is a no-op.
   Picture rotation/waver effects are not drawn.
7. **Manual save** (Dreamcast addition): the Memory Card window's "A: Save now"
   runs the game's own SAVE GAME event (CE26), the same thing a bed does,
   including its side effects (e.g. switch 106 when switch 104 is on).
8. **Not reproduced (EXE-only, not in the data)**: footstep sounds
   (`step_*` files exist but nothing in the data plays them - the modified
   RPG_RT.exe probably did), the alternative title screens `title_stay` /
   `title_dead`, the `m_*` menu sounds. Their trigger rules live in the
   obfuscated executable, so they were left out rather than guessed.

## Regenerating

```
./build/host/unit_tests --registry > docs/generated/bridge_registry.json
python3 tools/bridge_inventory.py "$GAME" docs/generated/bridge_registry.json docs/generated/compatibility_inventory.json
python3 tools/command_coverage.py "$GAME" --json docs/generated/command_coverage.json
```
