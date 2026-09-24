#!/bin/sh
# Regenerates the Splat disassembly workspace (asm/, assets/). Both are ignored.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
SPLAT_BIN=${SPLAT_BIN:-$ROOT/.tools-venv/bin/splat}
require_rom

if [ ! -x "$SPLAT_BIN" ]; then
    echo "Splat is missing: $SPLAT_BIN"
    echo "Run ./scripts/bootstrap_decomp.sh first"
    exit 2
fi

touch "$ROOT/decomp/symbol_addrs.txt" "$ROOT/decomp/reloc_addrs.txt"
echo "Splitting ROM segments"
cd "$ROOT" && "$SPLAT_BIN" split splat.yaml --disassemble-all --make-full-disasm-for-code
