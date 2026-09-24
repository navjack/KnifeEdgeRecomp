#!/bin/sh
# Turns Splat's discovered control-flow boundaries into a candidate symbol map.
#
# This is an EXPERIMENTAL recompiler probe, not game metadata. Function
# boundaries are Splat's guesses and data symbols are unresolved, so the output
# lives in decomp/ and generated-candidate/ and is gitignored. Never promote it
# to input/ without review.
#
# Sections come from splat.yaml, so adding an overlay there is enough -- this
# script needs no edit.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

if [ ! -d "$ROOT/asm" ]; then
    echo "Splat output is missing: $ROOT/asm"
    echo "Run ./scripts/bootstrap_decomp.sh and ./scripts/split_rom.sh first"
    exit 2
fi

python3 "$ROOT/tools/candidate-map/make_candidate_map.py" \
    --root "$ROOT" \
    --out "$ROOT/decomp/knife-edge.candidate.syms.toml"

echo "Probe it with: (cd config && ../build/N64Recomp/N64Recomp recomp-candidate.toml)"
