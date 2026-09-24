#!/bin/sh
# Verifies the pinned checkouts and patch fingerprints, applies the patches, and
# builds the shared arm64 stack into build/.
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
    actual=$(shasum -a 256 "$ROOT/patches/$1" | awk '{ print $1 }')
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
# This one lands in recompilation.cpp too, which the cache-noop patch above has
# already modified; apply_patch tracks that and checks it with git apply --check.
apply_patch N64Recomp N64Recomp-zero-register-writes.patch \
    'Writes to $zero have no effect' src/recompilation.cpp

COMMON_ARGS="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"

echo "Building N64Recomp"
"$CMAKE_BIN" -S "$ROOT/upstream/N64Recomp" -B "$ROOT/build/N64Recomp" $COMMON_ARGS
"$CMAKE_BIN" --build "$ROOT/build/N64Recomp" --parallel 8

echo "Building N64ModernRuntime"
"$CMAKE_BIN" -S "$ROOT/upstream/N64ModernRuntime" -B "$ROOT/build/N64ModernRuntime" $COMMON_ARGS \
    -DN64RECOMP_ROOT="$ROOT/upstream/N64Recomp"
"$CMAKE_BIN" --build "$ROOT/build/N64ModernRuntime" --parallel 8

echo "Building libultraship"
"$CMAKE_BIN" -S "$ROOT/upstream/libultraship" -B "$ROOT/build/libultraship" $COMMON_ARGS \
    -DLUS_BUILD_TESTS=OFF -DINCLUDE_MPQ_SUPPORT=OFF
"$CMAKE_BIN" --build "$ROOT/build/libultraship" --parallel 8

echo "Building RT64"
"$CMAKE_BIN" -S "$ROOT/upstream/rt64" -B "$ROOT/build/rt64" $COMMON_ARGS \
    '-DCMAKE_CXX_FLAGS=-include stdlib.h' -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
"$CMAKE_BIN" --build "$ROOT/build/rt64" --parallel 8

test -x "$ROOT/build/N64Recomp/N64Recomp"
test -f "$ROOT/build/N64ModernRuntime/ultramodern/libultramodern.a"
test -f "$ROOT/build/N64ModernRuntime/librecomp/liblibrecomp.a"
test -f "$ROOT/build/libultraship/src/libultraship.a"
test -f "$ROOT/build/rt64/rt64.dylib"

echo "Native arm64 stack is ready"
file "$ROOT/build/N64Recomp/N64Recomp" "$ROOT/build/rt64/rt64.dylib"
