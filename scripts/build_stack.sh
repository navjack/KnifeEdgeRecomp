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
apply_patch() {
    repo=$1
    patch=$2
    marker=$3
    file=$4
    verify_patch "$patch"
    if grep -qF "$marker" "$ROOT/upstream/$repo/$file"; then
        return
    fi
    if ! git -C "$ROOT/upstream/$repo" diff --quiet -- "$file"; then
        echo "Refusing to apply patches/$patch over existing local edits in $file"
        exit 2
    fi
    git -C "$ROOT/upstream/$repo" apply --check "$ROOT/patches/$patch"
    git -C "$ROOT/upstream/$repo" apply "$ROOT/patches/$patch"
    echo "applied patches/$patch"
}

apply_patch N64Recomp N64Recomp-cache-noop.patch \
    "N64 cache maintenance: no host-side operation" src/recompilation.cpp
apply_patch N64Recomp N64Recomp-cop0-registers.patch \
    "cop0_regs[32]" include/recomp.h
apply_patch N64Recomp N64Recomp-fpu64-conversions.patch \
    "cpu_trunc_l_s" src/operations.cpp
# recompilation.cpp already carries the cache-noop patch, so this one is checked
# by its own marker rather than by requiring a clean file.
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
