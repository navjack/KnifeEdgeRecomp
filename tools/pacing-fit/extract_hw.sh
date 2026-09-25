#!/usr/bin/env bash
# Per-frame change in the hardware longplay across stage 1's gameplay stretch:
# one YAVG (mean luma of the frame difference) per captured frame. A value under
# 0.3 is a repeated frame; at 30 fps of a 60 Hz console, every game step of two
# or more retraces shows as exactly one changed frame.
# Usage: extract_hw.sh <longplay.mp4> <out.txt>
set -euo pipefail
ffmpeg -loglevel error -ss 254 -t 170 -i "$1" \
  -vf "crop=960:720:160:0,scale=320:240,format=gray,tblend=all_mode=difference,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=$2" \
  -f null -
