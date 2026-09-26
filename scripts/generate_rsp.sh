#!/bin/sh
# Regenerates the recompiled RSP microcode from the configs in rsp/.
#
# The output is a mechanical translation of the game's own microcode, so like
# generated/ it is gitignored and never committed: only the tomls are. The
# build needs the ROM for generated/ anyway, and build_app.sh runs this after
# recompile.sh, so a checkout with the ROM always has it.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_rom

TOOL="$ROOT/build/N64Recomp/RSPRecomp$EXE"
if [ ! -x "$TOOL" ]; then
    echo "RSPRecomp is missing: $TOOL"
    echo "Run ./scripts/build_stack.sh first"
    exit 2
fi

found=0
for config in "$ROOT"/rsp/*.toml; do
    [ -e "$config" ] || continue
    found=1
    echo "=== $(basename "$config") ==="
    # RSPRecomp resolves rom_file_path and output_file_path relative to its own
    # working directory, so it has to run from rsp/.
    (cd "$ROOT/rsp" && "$TOOL" "$(basename "$config")")
done

if [ "$found" -eq 0 ]; then
    echo "No RSP configs in $ROOT/rsp"
    exit 2
fi

