#!/bin/sh
# Recompiles, configures and builds the native arm64 app, then packages it.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

"$SCRIPT_DIR/recompile.sh"

GENERATED="$ROOT/generated"
for required in funcs.h lookup.cpp recomp_overlays.inl; do
    if [ ! -f "$GENERATED/$required" ]; then
        echo "Generated game artifact is missing: $GENERATED/$required"
        exit 2
    fi
done

RSP_SOURCES=$(find "$ROOT/rsp" -type f \( -name '*.c' -o -name '*.cpp' \) | wc -l | tr -d ' ')
if [ "$RSP_SOURCES" -eq 0 ]; then
    echo "Title-specific RSP sources are missing: $ROOT/rsp"
    echo "The app must not run with an empty RSP implementation; see rsp/README.md."
    exit 2
fi

BUILD_DIR="$ROOT/build/app"
echo "Configuring native macOS app"
"$CMAKE_BIN" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_PREFIX_PATH=/opt/homebrew \
    -DKNIFE_EDGE_ROOT="$ROOT"
"$CMAKE_BIN" --build "$BUILD_DIR" --parallel 8

APP="$BUILD_DIR/${GAME_ID}.app"
test -d "$APP"
file "$APP/Contents/MacOS/${GAME_ID}"
"$SCRIPT_DIR/package_app.sh"
