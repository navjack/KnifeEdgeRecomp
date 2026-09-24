#!/bin/sh
# Static symbol-map recovery: name libultra functions by structural match against
# reviewed maps from other recomp projects, then report what is still unmapped.
#
# This does NOT write to input/. It operates on the candidate map and prints what
# it would adopt, because a wrong name is worse than no name: N64Recomp carries a
# 358-entry `ignored_funcs` set whose members are silently deleted from the build,
# and `osPiReadIo` is "reimplemented" with no runtime implementation at all. Only
# names backed by a real `_recomp` replacement are safe to keep.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_rom

TOOL="$ROOT/tools/map-recovery/map-recovery"
MAP=${MAP:-$ROOT/decomp/knife-edge.candidate.syms.toml}
REFS=${REFS:-"/Volumes/4terrybi/coding/Cruis'n"}

if [ ! -x "$TOOL" ]; then
    echo "Building map-recovery"
    (cd "$ROOT/tools/map-recovery" && go build -o map-recovery .)
fi

for game in cruisn-usa cruisn-world cruisn-exotica; do
    ref_map="$REFS/games/$game/input/$game.syms.toml"
    if [ ! -f "$ref_map" ]; then
        echo "skip $game (no reviewed map at $ref_map)"
        continue
    fi
    ref_rom=$(awk -F'"' '/^file = /{ print $2; exit }' "$REFS/games/$game/roms.toml" 2>/dev/null || true)
    case "$ref_rom" in
        ../../*) ref_rom="$REFS/${ref_rom#../../}" ;;
    esac
    if [ ! -f "$ref_rom" ]; then
        echo "skip $game (reference ROM not found: $ref_rom)"
        continue
    fi
    echo "=== match against $game ==="
    "$TOOL" match -ref-rom "$ref_rom" -ref-map "$ref_map" \
        -rom "$ROM" -map "$MAP" || true
done

echo
echo "=== sweep: entry points the map is still missing ==="
"$TOOL" sweep -rom "$ROM" -map "$MAP" || true

echo
echo "=== locate: any jal target outside every mapped section ==="
"$TOOL" locate -rom "$ROM" -map "$MAP" || true
