#!/bin/bash
# Drive Flycast headlessly: boot IMAGE under Xvfb, replay a timed key script,
# take screenshots.  Requires Xvfb, xdotool, ImageMagick (convert), xwd.
#   flycast_drive.sh FLYCAST IMAGE SCRIPT OUTDIR
# SCRIPT lines (times in seconds from boot):
#   <t> key <keysym> [hold_seconds]     press (and optionally hold) a key
#   <t> keys <k1+k2+...> <hold_seconds> hold several keys together (e.g. L+R = f+v)
#   <t> shot <name>                     save OUTDIR/<name>.png
#   <t> end
# Flycast default keyboard map: A=x B=c X=s Y=d START=Return, arrows=D-pad, L=f R=v
set -u
FLY="$1"; IMG="$2"; SCRIPT="$3"; OUT="$4"
mkdir -p "$OUT"
DISP=:78
Xvfb $DISP -screen 0 1280x1024x24 >/dev/null 2>&1 & XPID=$!
sleep 1
export DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1
"$FLY" "$IMG" > "$OUT/flycast.log" 2>&1 & FPID=$!
T0=$(date +%s.%N)
WIN=""
while read -r t act a b; do
  [[ -z "${t:-}" || "$t" == \#* ]] && continue
  now=$(date +%s.%N); wait=$(echo "$t - ($now - $T0)" | bc -l)
  if (( $(echo "$wait > 0" | bc -l) )); then sleep "$wait"; fi
  [[ -z "$WIN" ]] && WIN=$(xdotool search --onlyvisible --name "Flycast" 2>/dev/null | tail -1) && [[ -n "$WIN" ]] && xdotool windowfocus "$WIN" 2>/dev/null
  case "$act" in
    key) xdotool keydown "$a"; sleep "${b:-0.12}"; xdotool keyup "$a" ;;
    keys) IFS=+ read -ra K <<< "$a"; for k in "${K[@]}"; do xdotool keydown "$k"; done
          sleep "${b:-0.12}"; for k in "${K[@]}"; do xdotool keyup "$k"; done ;;
    shot) xwd -id "$WIN" -silent | convert xwd:- "$OUT/$a.png"; echo "shot $a at $t s" ;;
    end) break ;;
  esac
done < "$SCRIPT"
kill $FPID 2>/dev/null; sleep 1; kill -9 $FPID 2>/dev/null; kill $XPID 2>/dev/null
grep -vE "ALSA|^\s*$" "$OUT/flycast.log" | tail -5
