#!/bin/sh
# Runs N64Recomp against the reviewed metadata in input/ and writes generated/.
# It refuses to invent game metadata: with no ELF and no symbol map it stops.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
RECOMP=${N64RECOMP_BIN:-$ROOT/build/N64Recomp/N64Recomp$EXE}
CANDIDATE=0
if [ "${1:-}" = "--candidate" ]; then
    CANDIDATE=1
    shift
fi
ELF="$ROOT/input/knife-edge.elf"
SYMBOLS="$ROOT/input/knife-edge.syms.toml"
GENERATED="$ROOT/generated"

require_rom

if [ ! -x "$RECOMP" ]; then
    echo "N64Recomp is missing: $RECOMP"
    echo "Run ./scripts/build_stack.sh first"
    exit 2
fi

if [ "$CANDIDATE" -eq 1 ]; then
    # Development path: build from the UNREVIEWED candidate map so the runtime
    # can be exercised before a reviewed map exists. input/ is deliberately left
    # alone -- promoting the candidate there would erase the distinction between
    # metadata that has been checked and metadata that has not.
    echo "############################################################"
    echo "# CANDIDATE BUILD -- boundaries are Splat's guesses, refined"
    echo "# by tools/candidate-map/converge.py. Not reviewed metadata."
    echo "# Wrong behaviour here is expected and proves nothing about"
    echo "# the game. Do not report results from it as a milestone."
    echo "############################################################"
    CONFIG="$ROOT/config/recomp-candidate.toml"
    cd "$ROOT/config" && "$RECOMP" "$CONFIG"
    rm -rf "$GENERATED"
    cp -R "$ROOT/generated-candidate" "$GENERATED"
    date > "$GENERATED/CANDIDATE_BUILD"
    python3 "$ROOT/tools/candidate-map/fix_dangling_labels.py" "$GENERATED"
    grep -ohE '\b[A-Za-z_][A-Za-z0-9_]*_recomp\(rdram, ctx\)' "$GENERATED"/*.c \
        | sed 's/(rdram, ctx)$//' \
        | sort -u \
        | awk 'BEGIN { print "#pragma once"; print "#include \"recomp.h\"" }
               { printf "void %s(uint8_t* rdram, recomp_context* ctx);\n", $0 }' \
        > "$GENERATED/recomp_decls.h"
    echo "Generated output: $GENERATED (candidate)"
    exit 0
fi

if [ -f "$ELF" ]; then
    CONFIG="$ROOT/config/recomp.toml"
elif [ -f "$SYMBOLS" ]; then
    CONFIG="$ROOT/config/recomp-symbols.toml"
else
    echo "Missing game metadata: $ELF or $SYMBOLS"
    echo
    echo "N64Recomp cannot produce valid native code from a raw ROM alone. A ROM"
    echo "has bytes but no function boundaries, relocations, data symbols or"
    echo "overlay information. Supply the matching MIPS ELF or a complete,"
    echo "reviewed symbol map, then rerun."
    echo
    echo "To produce an unreviewed probe instead, see"
    echo "./scripts/generate_candidate_symbols.sh"
    exit 2
fi

echo "Recompiling with $(basename "$CONFIG")"
cd "$ROOT/config" && "$RECOMP" "$CONFIG"

# Generated code calls runtime replacements named <symbol>_recomp, but funcs.h
# only declares some of them; declare every one the output actually calls.
grep -ohE '\b[A-Za-z_][A-Za-z0-9_]*_recomp\(rdram, ctx\)' "$GENERATED"/*.c \
    | sed 's/(rdram, ctx)$//' \
    | sort -u \
    | awk 'BEGIN { print "#pragma once"; print "#include \"recomp.h\"" }
           { printf "void %s(uint8_t* rdram, recomp_context* ctx);\n", $0 }' \
    > "$GENERATED/recomp_decls.h"

echo "Generated output: $GENERATED"
