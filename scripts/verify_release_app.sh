#!/bin/sh
# Static release check. It does not launch the game: gameplay, rendering, audio
# and input are validated by a person playing it.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

APP="$ROOT/$DISPLAY_NAME.app"
EXECUTABLE="$APP/Contents/MacOS/$GAME_ID"
RESOURCES="$APP/Contents/Resources"

failures=0
check() {
    if eval "$2"; then
        echo "ok    $1"
    else
        echo "FAIL  $1"
        failures=$((failures + 1))
    fi
}

check "bundle exists" '[ -d "$APP" ]'
check "arm64 executable" 'lipo -archs "$EXECUTABLE" 2>/dev/null | grep -qw arm64'
check "frontend stylesheet" '[ -f "$RESOURCES/assets/recomp.rcss" ]'
check "primary font" '[ -f "$RESOURCES/assets/InterVariable.ttf" ]'
check "prompt font" '[ -f "$RESOURCES/assets/promptfont/promptfont.ttf" ]'
check "controller database" '[ -f "$RESOURCES/recompcontrollerdb.txt" ]'
check "bundled SDL3" '[ -f "$APP/Contents/Frameworks/libSDL3.dylib" ]'
check "no data files in MacOS" '[ "$(find "$APP/Contents/MacOS" -type f ! -perm -u+x | wc -l | tr -d " ")" = "0" ]'
check "no Homebrew links" '! otool -L "$EXECUTABLE" "$APP"/Contents/Frameworks/*.dylib | grep -q /opt/homebrew'
check "no Homebrew rpath" '! otool -l "$EXECUTABLE" | awk "\$1 == \"path\" { print \$2 }" | grep -q /opt/homebrew'
check "valid signature" 'codesign --verify --deep --strict "$APP" 2>/dev/null'

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed for $APP"
    exit 1
fi
echo "Static release checks passed: $APP"
