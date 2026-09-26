#!/bin/sh
# Builds everything the app needs that does not come from the ROM: every library
# it links, and the port's own sources compiled against them. No executable is
# produced, because what would complete the link -- generated/ and the
# recompiled audio microcode -- is made from the ROM. This is as far as a
# machine without the ROM can go, and it is what CI runs on each platform.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

# The port compiles against the patched runtime headers (cop0_regs, the
# instruction counter), so the stack has to have been through build_stack.sh.
if ! grep -qF "cop0_regs[32]" "$ROOT/upstream/N64Recomp/include/recomp.h" 2>/dev/null; then
    echo "Upstream checkouts are missing or unpatched under: $ROOT/upstream"
    echo "Run ./scripts/bootstrap.sh and ./scripts/build_stack.sh first"
    exit 2
fi

PREFIX_ARGS=""
if [ "$HOST_OS" = macos ]; then
    PREFIX_ARGS="-DCMAKE_PREFIX_PATH=/opt/homebrew"
fi

BUILD_DIR="$ROOT/build/check"
echo "Configuring the ROM-free check ($HOST_OS)"
"$CMAKE_BIN" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    $PLATFORM_CMAKE_ARGS $PREFIX_ARGS \
    -DKNIFE_EDGE_ROOT="$ROOT" \
    -DKNIFE_EDGE_CHECK_ONLY=ON
"$CMAKE_BIN" --build "$BUILD_DIR" --target "${GAME_ID}_check" --parallel "$JOBS"

echo "ROM-free check passed ($HOST_OS): libraries built, port sources compiled."
echo "Not checked without the ROM: generated code, recompiled RSP, the final link."
