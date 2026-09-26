#!/bin/sh
# Recompiles, configures and builds the native arm64 app, then packages it.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

CANDIDATE_ARG=""
if [ "${1:-}" = "--candidate" ]; then
    CANDIDATE_ARG="--candidate"
fi
"$SCRIPT_DIR/recompile.sh" $CANDIDATE_ARG

GENERATED="$ROOT/generated"
for required in funcs.h lookup.cpp recomp_overlays.inl; do
    if [ ! -f "$GENERATED/$required" ]; then
        echo "Generated game artifact is missing: $GENERATED/$required"
        exit 2
    fi
done

"$SCRIPT_DIR/generate_rsp.sh"
if [ ! -f "$ROOT/rsp/aspMain.cpp" ]; then
    echo "Recompiled RSP microcode is missing: $ROOT/rsp/aspMain.cpp"
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

if [ -f "$GENERATED/CANDIDATE_BUILD" ]; then
    echo
    echo "NOTE: built from the unreviewed candidate map ($(cat "$GENERATED/CANDIDATE_BUILD"))."
    echo "      Treat any run of this bundle as a runtime smoke test only."
fi

APP="$BUILD_DIR/${GAME_ID}.app"
test -d "$APP"
file "$APP/Contents/MacOS/${GAME_ID}"
"$SCRIPT_DIR/package_app.sh"
