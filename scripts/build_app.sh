#!/bin/sh
# Recompiles, configures and builds the native app. On macOS it then packages
# the arm64 bundle; on Linux and Windows the build directory runs in place.
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

PREFIX_ARGS=""
if [ "$HOST_OS" = macos ]; then
    PREFIX_ARGS="-DCMAKE_PREFIX_PATH=/opt/homebrew"
fi

BUILD_DIR="$ROOT/build/app"
echo "Configuring native app ($HOST_OS)"
"$CMAKE_BIN" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    $PLATFORM_CMAKE_ARGS $PREFIX_ARGS \
    -DKNIFE_EDGE_ROOT="$ROOT" \
    -DKNIFE_EDGE_CHECK_ONLY=OFF
"$CMAKE_BIN" --build "$BUILD_DIR" --parallel "$JOBS"

if [ -f "$GENERATED/CANDIDATE_BUILD" ]; then
    echo
    echo "NOTE: built from the unreviewed candidate map ($(cat "$GENERATED/CANDIDATE_BUILD"))."
    echo "      Treat any run of this bundle as a runtime smoke test only."
fi

if [ "$HOST_OS" = macos ]; then
    APP="$BUILD_DIR/${GAME_ID}.app"
    test -d "$APP"
    file "$APP/Contents/MacOS/${GAME_ID}"
    "$SCRIPT_DIR/package_app.sh"
else
    # Linux and Windows run in place: the build puts assets, the controller
    # database and (on Windows) SDL2 and DXC beside the executable.
    test -f "$BUILD_DIR/${GAME_ID}$EXE"
    echo "Built: $BUILD_DIR/${GAME_ID}$EXE"
fi
