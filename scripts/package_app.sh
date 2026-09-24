#!/bin/sh
# Packages the built app as a self-contained "<Display Name>.app" in the
# workspace root: frontend assets, vendored Homebrew dylibs, ad-hoc signature.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"

BUILD_APP="$ROOT/build/app/${GAME_ID}.app"
RELEASE_APP="$ROOT/$DISPLAY_NAME.app"
FRONTEND="$ROOT/native/frontend"
BREW_PREFIX=/opt/homebrew
SDL3_LIBRARY="$BREW_PREFIX/opt/sdl3/lib/libSDL3.0.dylib"

for required in "$BUILD_APP" "$FRONTEND/assets" "$FRONTEND/recompcontrollerdb.txt" "$SDL3_LIBRARY"; do
    if [ ! -e "$required" ]; then
        echo "Required packaging input is missing: $required"
        exit 2
    fi
done

rm -rf "$RELEASE_APP"
ditto "$BUILD_APP" "$RELEASE_APP"

CONTENTS="$RELEASE_APP/Contents"
EXECUTABLE="$CONTENTS/MacOS/$GAME_ID"
RESOURCES="$CONTENTS/Resources"
FRAMEWORKS="$CONTENTS/Frameworks"
mkdir -p "$RESOURCES" "$FRAMEWORKS"

# Data files belong in Resources: codesign treats any non-Mach-O file in
# Contents/MacOS as an unsigned nested code component and refuses to seal.
rm -rf "$RESOURCES/assets"
ditto "$FRONTEND/assets" "$RESOURCES/assets"
cp "$FRONTEND/recompcontrollerdb.txt" "$RESOURCES/recompcontrollerdb.txt"

PLIST="$CONTENTS/Info.plist"
set_plist() { plutil -replace "$1" -"$2" "$3" "$PLIST"; }
set_plist CFBundleName string "$DISPLAY_NAME"
set_plist CFBundleDisplayName string "$DISPLAY_NAME"
set_plist CFBundleShortVersionString string 0.1.0
set_plist CFBundleVersion string 0.1.0
set_plist LSMinimumSystemVersion string 13.0
set_plist NSHighResolutionCapable bool true
set_plist LSApplicationCategoryType string public.app-category.games

# Copy every Homebrew dylib a binary links against into Frameworks, repoint the
# binary at the bundled copy, and repeat for that copy's own links.
vendor_dependencies() {
    local binary dependency library
    binary=$1
    otool -L "$binary" | awk 'NR > 1 { print $1 }' | grep "^$BREW_PREFIX/" | while read -r dependency; do
        library=$(basename "$dependency")
        if [ ! -f "$FRAMEWORKS/$library" ]; then
            cp "$dependency" "$FRAMEWORKS/$library"
            chmod u+w "$FRAMEWORKS/$library"
            install_name_tool -id "@rpath/$library" "$FRAMEWORKS/$library" 2>/dev/null
            install_name_tool -add_rpath @loader_path "$FRAMEWORKS/$library" 2>/dev/null || true
            vendor_dependencies "$FRAMEWORKS/$library"
        fi
        install_name_tool -change "$dependency" "@rpath/$library" "$binary" 2>/dev/null
    done
}
vendor_dependencies "$EXECUTABLE"

# sdl2-compat dlopens SDL3 by name rather than through a link entry.
cp "$SDL3_LIBRARY" "$FRAMEWORKS/libSDL3.dylib"
chmod u+w "$FRAMEWORKS/libSDL3.dylib"
install_name_tool -id "@rpath/libSDL3.dylib" "$FRAMEWORKS/libSDL3.dylib" 2>/dev/null

# dyld searches LC_RPATHs in order, so an absolute Homebrew rpath left in place
# wins over the vendored copies and otool -L still looks correct.
otool -l "$EXECUTABLE" | awk '$1 == "path" { print $2 }' | grep "^$BREW_PREFIX" | while read -r rpath; do
    install_name_tool -delete_rpath "$rpath" "$EXECUTABLE"
done
if ! otool -l "$EXECUTABLE" | awk '$1 == "path" { print $2 }' | grep -qx '@executable_path/../Frameworks'; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXECUTABLE"
fi

# install_name_tool invalidates arm64 signatures, so sign after rewriting.
codesign --force --sign - --timestamp=none "$FRAMEWORKS"/*.dylib >/dev/null
codesign --force --deep --sign - --timestamp=none "$RELEASE_APP" >/dev/null

if otool -L "$EXECUTABLE" "$FRAMEWORKS"/*.dylib | grep -q "$BREW_PREFIX"; then
    echo "Release app still links against Homebrew: $RELEASE_APP"
    exit 1
fi
if ! codesign --verify --deep --strict "$RELEASE_APP"; then
    echo "Release app signature verification failed: $RELEASE_APP"
    exit 1
fi

echo "Packaged self-contained app: $RELEASE_APP"
file "$EXECUTABLE"
