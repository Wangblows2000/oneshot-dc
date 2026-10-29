#!/bin/bash
# Run every scripted gameplay test against converted data with the headless runner.
set -u
DATA="${1:-build/cdroot/data}"
OUT="${2:-build/test_shots}"
RUN=./build/host/host_runner
mkdir -p "$OUT"
fail=0
run() { # name script frames [extra args]
  local name="$1" script="$2" frames="$3"; shift 3
  rm -rf build/host_vmu_test
  if "$RUN" "$DATA" "$script" --frames "$frames" --shots "$OUT" --store build/host_vmu_test "$@" > "$OUT/$name.log" 2>&1; then
    echo "PASS  $name   ($(grep -c PASS "$OUT/$name.log") checks)"
  else
    echo "FAIL  $name   (see $OUT/$name.log)"; grep FAIL "$OUT/$name.log" | head -5; fail=1
  fi
  grep -E "unsupported command kinds" "$OUT/$name.log" | sed 's/^/      /'
}
run opening            tests/scripts/opening.txt            1100
run safe_puzzle        tests/scripts/safe_puzzle.txt        14000
run restart_persistence tests/scripts/restart_persistence.txt 12000
run no_vmu             tests/scripts/no_vmu.txt             12000 --no-vmu

run held_button        tests/scripts/held_button.txt        1100
run remote_window      tests/scripts/remote_window.txt      14000
run combinations       build/generated/tests/combinations.txt 6000
run item_puzzles       build/generated/tests/item_puzzles.txt 9000
run debug_menu         build/generated/tests/debug_menu.txt   12000
run movement           tests/scripts/movement.txt           3000
run page_conditions    tests/scripts/page_conditions.txt    1500
run battle_anim        tests/scripts/battle_anim.txt        5000
run alula_puzzle       build/generated/tests/alula.txt      6000
run elevator           build/generated/tests/elevator.txt   8000
run panorama           tests/scripts/panorama.txt           12000
run manual_save        tests/scripts/manual_save.txt        8000
# the power-cycle check reuses the card written by manual_save
if "$RUN" "$DATA" tests/scripts/manual_save_powercycle.txt --frames 4000 --store build/host_vmu_test > "$OUT/manual_save_powercycle.log" 2>&1; then
  echo "PASS  manual_save_powercycle   ($(grep -c PASS "$OUT/manual_save_powercycle.log") checks)"
else echo "FAIL  manual_save_powercycle"; fail=1; fi
python3 -I tests/gen/check_clue.py "$DATA" > "$OUT/check_clue.log" 2>&1 && echo "PASS  clue_wallpaper_vs_floor" || { echo "FAIL  clue_wallpaper_vs_floor"; fail=1; }
exit $fail
