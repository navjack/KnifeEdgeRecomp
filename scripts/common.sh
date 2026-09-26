# Shared definitions for the Knife Edge scripts. Sourced, never executed.

# macos, linux or windows. Windows means Git Bash or MSYS2 driving the native
# clang-cl toolchain; the scripts themselves stay POSIX sh on every host.
case "$(uname -s)" in
    Darwin) HOST_OS=macos ;;
    Linux) HOST_OS=linux ;;
    MINGW* | MSYS* | CYGWIN*) HOST_OS=windows ;;
    *)
        echo "Unsupported host: $(uname -s)" >&2
        exit 2
        ;;
esac

# On Windows, paths handed to native tools (cmake, the compilers, N64Recomp)
# must be drive-letter paths; `pwd -W` gives C:/... where plain pwd gives /c/...
# With every path already native, MSYS's own argument rewriting can only do
# harm: it would turn clang-cl's /FIstdlib.h into a path under the Git install.
if [ "$HOST_OS" = windows ]; then
    export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
fi
native_pwd() {
    if [ "$HOST_OS" = windows ]; then pwd -W; else pwd; fi
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && native_pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && native_pwd)
LOCK_FILE="$ROOT/UPSTREAM_PINS.txt"

EXE=""
if [ "$HOST_OS" = windows ]; then
    EXE=.exe
fi

GAME_ID=knife_edge
DISPLAY_NAME="Knife Edge Nose Gunner"
ROM="$ROOT/rom/Knife Edge - Nose Gunner (U) [!].z64"
ROM_SHA256=467fcb503f384d2479fb3a5fe22405089f56ecab38a3acd1a6aaf0d8b73372b5

CMAKE_BIN=${CMAKE_BIN:-/opt/homebrew/bin/cmake}
[ -x "$CMAKE_BIN" ] || CMAKE_BIN=cmake

JOBS=${JOBS:-${NUMBER_OF_PROCESSORS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}}

# Toolchain arguments for every CMake configure. All three hosts build with
# clang -- Apple clang, clang, clang-cl -- so the generated code only ever meets
# one compiler family. CC and CXX override the Linux choice.
case "$HOST_OS" in
    macos)
        PLATFORM_CMAKE_ARGS="-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"
        ;;
    linux)
        PLATFORM_CMAKE_ARGS="-DCMAKE_C_COMPILER=${CC:-clang} -DCMAKE_CXX_COMPILER=${CXX:-clang++}"
        ;;
    windows)
        # Needs an MSVC developer environment (vcvars64) for the SDK and CRT.
        PLATFORM_CMAKE_ARGS="-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link"
        ;;
esac

# SHA-256 of a file. macOS ships shasum, most Linux images ship sha256sum, Git
# Bash ships both.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    else
        shasum -a 256 "$1" | awk '{ print $1 }'
    fi
}

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
    actual=$(sha256_of "$ROM")
    if [ "$actual" != "$ROM_SHA256" ]; then
        echo "ROM fingerprint mismatch"
        echo "  expected: $ROM_SHA256"
        echo "  actual:   $actual"
        exit 2
    fi
}
