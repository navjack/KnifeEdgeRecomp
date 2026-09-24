# Shared definitions for the Knife Edge scripts. Sourced, never executed.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
LOCK_FILE="$ROOT/UPSTREAM_PINS.txt"

GAME_ID=knife_edge
DISPLAY_NAME="Knife Edge Nose Gunner"
ROM="$ROOT/rom/Knife Edge - Nose Gunner (U) [!].z64"
ROM_SHA256=467fcb503f384d2479fb3a5fe22405089f56ecab38a3acd1a6aaf0d8b73372b5

CMAKE_BIN=${CMAKE_BIN:-/opt/homebrew/bin/cmake}
[ -x "$CMAKE_BIN" ] || CMAKE_BIN=cmake

# Reads "key = value" from the lock file. The separator is matched as a regex
# with surrounding whitespace so the key never keeps a trailing space.
lock_value() {
    awk -F' *= *' -v wanted_key="$1" '$1 == wanted_key { print $2; exit }' "$LOCK_FILE"
}

# Fails loudly rather than handing an empty string to git, which reports it as
# "fatal: The empty string is not a valid path" far from the real cause.
require_lock_value() {
    value=$(lock_value "$1")
    if [ -z "$value" ]; then
        echo "Missing key in $LOCK_FILE: $1" >&2
        exit 2
    fi
    printf '%s' "$value"
}

require_lock_file() {
    if [ ! -f "$LOCK_FILE" ]; then
        echo "Dependency lock is missing: $LOCK_FILE"
        exit 2
    fi
}

# The recompiler must never be pointed at a ROM other than the supported dump.
require_rom() {
    if [ ! -f "$ROM" ]; then
        echo "ROM is missing: $ROM"
        exit 2
    fi
    actual=$(shasum -a 256 "$ROM" | awk '{ print $1 }')
    if [ "$actual" != "$ROM_SHA256" ]; then
        echo "ROM fingerprint mismatch"
        echo "  expected: $ROM_SHA256"
        echo "  actual:   $actual"
        exit 2
    fi
}
