#!/bin/sh
# Fetches every pinned upstream into upstream/, recursively.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_lock_file

checkout_pinned() {
    name=$1
    url=$2
    revision=$3
    target="$ROOT/upstream/$name"

    if [ -e "$target" ]; then
        if [ ! -d "$target/.git" ]; then
            echo "Existing path is not a Git checkout: $target"
            exit 2
        fi
        actual=$(git -C "$target" rev-parse HEAD)
        if [ "$actual" != "$revision" ]; then
            echo "$name is at $actual; expected $revision"
            echo "Refusing to alter an existing checkout. Resolve it manually, then rerun."
            exit 2
        fi
        git -C "$target" submodule update --init --recursive
        echo "verified $name @ $revision"
        return
    fi

    git clone --recurse-submodules "$url" "$target"
    git -C "$target" checkout --detach "$revision"
    git -C "$target" submodule update --init --recursive
    echo "cloned $name @ $revision"
}

for name in N64Recomp N64ModernRuntime rt64 hlslpp libultraship RecompFrontend; do
    checkout_pinned "$name" "$(lock_value "$name.url")" "$(lock_value "$name.commit")"
done

echo "All pinned upstream checkouts are ready"
