#!/bin/sh
# Turns Splat's discovered control-flow boundaries into a candidate symbol map.
#
# This is an EXPERIMENTAL recompiler probe, not game metadata. Overlays,
# relocations and data symbols are unresolved, so its output lives in
# decomp/ and generated-candidate/ and is gitignored. Never promote it to
# input/ without review.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
ASM="$ROOT/asm"
OUT="$ROOT/decomp/knife-edge.candidate.syms.toml"

# Boundaries from docs/ROM_ANALYSIS.md.
ENTRY_VRAM=0x800C2400
ENTRY_SIZE=0x38
# name rom vram size splat_file
SEGMENTS="main 0x1050 0x800C2450 0x261B0 1050.s"

for file in 1000.s $(printf '%s\n' "$SEGMENTS" | awk '{ print $5 }'); do
    if [ ! -f "$ASM/$file" ]; then
        echo "Splat output is missing: $ASM/$file"
        echo "Run ./scripts/bootstrap_decomp.sh and ./scripts/split_rom.sh first"
        exit 2
    fi
done

{
    echo "# Candidate generated from Splat control-flow boundaries."
    echo "# This is NOT a reviewed symbol map: overlays, relocations and data"
    echo "# symbols remain unresolved."
    echo
    echo "[[section]]"
    echo "name = \"entry\""
    echo "rom = 0x00001000"
    echo "vram = $ENTRY_VRAM"
    echo "size = $ENTRY_SIZE"
    echo "functions = [{ name = \"entrypoint\", vram = $ENTRY_VRAM, size = $ENTRY_SIZE }]"
    printf '%s\n' "$SEGMENTS" | while read -r name rom vram size file; do
        echo
        echo "[[section]]"
        echo "name = \"$name\""
        echo "rom = $rom"
        echo "vram = $vram"
        echo "size = $size"
        echo "functions = ["
        # A function's address is the VRAM field of its first instruction
        # comment, /* ROM VRAM WORD */. The game's own main is renamed so it
        # cannot collide with the host program's main.
        awk '
            /^nonmatching / {
                name = $2
                sub(/,$/, "", name)
                size = $3
                pending = 1
                next
            }
            pending && $1 == "/*" {
                if (name == "main") {
                    name = "func_" $3
                }
                printf "    { name = \"%s\", vram = 0x%s, size = %s },\n", name, $3, size
                pending = 0
            }
        ' "$ASM/$file"
        echo "]"
    done
} > "$OUT"

echo "Wrote candidate symbols: $OUT"
echo "Probe it with: build/N64Recomp/N64Recomp config/recomp-candidate.toml"
