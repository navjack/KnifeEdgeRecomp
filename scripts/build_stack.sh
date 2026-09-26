#!/bin/sh
# Verifies the pinned checkouts and patch fingerprints, applies the patches, and
# builds the shared stack into build/: arm64 on macOS, the host architecture on
# Linux and Windows.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_lock_file

if [ ! -d "$ROOT/upstream/N64Recomp" ]; then
    echo "Missing upstream checkouts under: $ROOT/upstream"
    echo "Run ./scripts/bootstrap.sh first"
    exit 2
fi

verify_checkout() {
    name=$1
    expected=$(require_lock_value "$name.commit")
    checkout="$ROOT/upstream/$name"
    if [ ! -d "$checkout/.git" ]; then
        echo "Missing locked checkout: $checkout"
        exit 2
    fi
    actual=$(git -C "$checkout" rev-parse HEAD)
    if [ "$actual" != "$expected" ]; then
        echo "$name is at $actual; expected $expected"
        exit 2
    fi
}

verify_patch() {
    expected=$(require_lock_value "patches/$1.sha256")
    actual=$(sha256_of "$ROOT/patches/$1")
    if [ "$actual" != "$expected" ]; then
        echo "Patch fingerprint mismatch: patches/$1"
        exit 2
    fi
}

for name in N64Recomp N64ModernRuntime rt64 hlslpp libultraship RecompFrontend; do
    verify_checkout "$name"
done

# Each patch is applied only when its marker string is absent, so re-running
# this script is a no-op, and never over unrelated local edits.
#
# Two patches can legitimately touch the same file, so once this run has patched
# a file, that file is no longer expected to be clean. Only files this run has
# not touched get the clean-tree check.
PATCHED_FILES=""

apply_patch() {
    repo=$1
    patch=$2
    marker=$3
    file=$4
    verify_patch "$patch"
    if grep -qF "$marker" "$ROOT/upstream/$repo/$file"; then
        # Already applied, from this run or an earlier one. Still record the
        # file: a later patch to the same file must not be refused for the edit
        # this one made.
        PATCHED_FILES="$PATCHED_FILES $repo/$file"
        return
    fi
    case " $PATCHED_FILES " in
        *" $repo/$file "*) ;;
        *)
            if ! git -C "$ROOT/upstream/$repo" diff --quiet -- "$file"; then
                echo "Refusing to apply patches/$patch over existing local edits in $file"
                exit 2
            fi
            ;;
    esac
    git -C "$ROOT/upstream/$repo" apply --check "$ROOT/patches/$patch"
    git -C "$ROOT/upstream/$repo" apply "$ROOT/patches/$patch"
    PATCHED_FILES="$PATCHED_FILES $repo/$file"
    echo "applied patches/$patch"
}

apply_patch N64Recomp N64Recomp-cache-noop.patch \
    "N64 cache maintenance: no host-side operation" src/recompilation.cpp
apply_patch N64Recomp N64Recomp-cop0-registers.patch \
    "cop0_regs[32]" include/recomp.h
apply_patch N64Recomp N64Recomp-fpu64-conversions.patch \
    "cpu_trunc_l_s" src/operations.cpp
# Sits just after the cache no-op in the same switch, so it applies after it.
apply_patch N64Recomp N64Recomp-cop0-control-trap.patch \
    "cpu_INVALID" src/recompilation.cpp
apply_patch N64Recomp N64Recomp-discard-bad-statics.patch \
    "discarding static function" src/main.cpp

# Only the FR bit has a host-side effect after static recompilation; exiting
# over any other status write kills the game during ordinary libultra startup.
# Emits RECOMP_COUNT_INSTR() beside every recompiled instruction -- a no-op
# unless the port defines it -- so the port can measure the R4300 work each
# game step does, the basis for pacing the game the way the console did.
apply_patch N64Recomp N64Recomp-instruction-counter.patch \
    "RECOMP_COUNT_INSTR" src/recompilation.cpp
apply_patch N64ModernRuntime N64ModernRuntime-cop0-status-tolerance.patch \
    "unhandled status register bits changed" librecomp/src/recomp.cpp
# This one lands in recompilation.cpp too, which the cache-noop patch above has
# already modified; apply_patch tracks that and checks it with git apply --check.
apply_patch N64Recomp N64Recomp-zero-register-writes.patch \
    'Writes to $zero have no effect' src/recompilation.cpp

COMMON_ARGS="-G Ninja -DCMAKE_BUILD_TYPE=Release $PLATFORM_CMAKE_ARGS"

# RT64 has always been built here with stdlib.h forced in; clang-cl spells the
# forced include differently.
if [ "$HOST_OS" = windows ]; then
    RT64_FORCE_INCLUDE=/FIstdlib.h
else
    RT64_FORCE_INCLUDE="-include stdlib.h"
fi

echo "Building N64Recomp"
"$CMAKE_BIN" -S "$ROOT/upstream/N64Recomp" -B "$ROOT/build/N64Recomp" $COMMON_ARGS
"$CMAKE_BIN" --build "$ROOT/build/N64Recomp" --parallel "$JOBS"

echo "Building N64ModernRuntime"
"$CMAKE_BIN" -S "$ROOT/upstream/N64ModernRuntime" -B "$ROOT/build/N64ModernRuntime" $COMMON_ARGS \
    -DN64RECOMP_ROOT="$ROOT/upstream/N64Recomp"
"$CMAKE_BIN" --build "$ROOT/build/N64ModernRuntime" --parallel "$JOBS"

# Nothing in this port links libultraship; it is here because the Cruis'n pins
# were verified as a set on macOS. Elsewhere it would pull in SDL3, OpenGL and
# vcpkg to produce a library no target uses, so it is only built on macOS.
if [ "$HOST_OS" = macos ]; then
    echo "Building libultraship"
    "$CMAKE_BIN" -S "$ROOT/upstream/libultraship" -B "$ROOT/build/libultraship" $COMMON_ARGS \
        -DLUS_BUILD_TESTS=OFF -DINCLUDE_MPQ_SUPPORT=OFF
    "$CMAKE_BIN" --build "$ROOT/build/libultraship" --parallel "$JOBS"
fi

echo "Building RT64"
"$CMAKE_BIN" -S "$ROOT/upstream/rt64" -B "$ROOT/build/rt64" $COMMON_ARGS \
    "-DCMAKE_CXX_FLAGS=$RT64_FORCE_INCLUDE" -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
"$CMAKE_BIN" --build "$ROOT/build/rt64" --parallel "$JOBS"

case "$HOST_OS" in
    macos) static_lib() { echo "$1/lib$2.a"; }; RT64_LIBRARY=rt64.dylib ;;
    linux) static_lib() { echo "$1/lib$2.a"; }; RT64_LIBRARY=rt64.so ;;
    windows) static_lib() { echo "$1/$2.lib"; }; RT64_LIBRARY=rt64.dll ;;
esac

test -x "$ROOT/build/N64Recomp/N64Recomp$EXE"
test -x "$ROOT/build/N64Recomp/RSPRecomp$EXE"
test -f "$(static_lib "$ROOT/build/N64ModernRuntime/ultramodern" ultramodern)"
test -f "$(static_lib "$ROOT/build/N64ModernRuntime/librecomp" librecomp)"
if [ "$HOST_OS" = macos ]; then
    test -f "$(static_lib "$ROOT/build/libultraship/src" libultraship)"
fi
test -f "$ROOT/build/rt64/$RT64_LIBRARY"

echo "Native stack is ready ($HOST_OS)"
if command -v file >/dev/null 2>&1; then
    file "$ROOT/build/N64Recomp/N64Recomp$EXE" "$ROOT/build/rt64/$RT64_LIBRARY"
fi
