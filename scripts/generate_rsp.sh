#!/bin/sh
# Regenerates the recompiled RSP microcode from the configs in rsp/.
#
# Unlike the candidate map, the output here IS committed: it is derived from
# the ROM, which is not, so a checkout with no ROM still has to build. This
# script exists so the committed file is reproducible rather than a artifact
# nobody can regenerate -- run it after changing a toml, and commit the result.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_rom

TOOL="$ROOT/build/N64Recomp/RSPRecomp"
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

echo
echo "Regenerated. Diff before committing: a change here that you did not"
echo "intend means the toml or the ROM moved, not that the microcode did."
