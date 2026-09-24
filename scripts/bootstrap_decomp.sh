#!/bin/sh
# Creates the Python venv that hosts Splat.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
PYTHON_BIN=${PYTHON_BIN:-python3}
VENV="$ROOT/.tools-venv"

if [ ! -x "$VENV/bin/python" ]; then
    "$PYTHON_BIN" -m venv "$VENV"
fi

"$VENV/bin/python" -m pip install --quiet --upgrade pip
"$VENV/bin/python" -m pip install --quiet 'splat64[mips]==0.50.0'
"$VENV/bin/splat" --version
