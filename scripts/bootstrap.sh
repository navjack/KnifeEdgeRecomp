#!/bin/sh
# Fetches every pinned upstream into upstream/, recursively.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_lock_file

# The patches in patches/ are LF, and git apply will not match them against a
# checkout that autocrlf has rewritten to CRLF, as Git for Windows does by
# default. Settings given with -c reach the submodule clones too.
GIT="git -c core.autocrlf=false"

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
        $GIT -C "$target" submodule update --init --recursive
        echo "verified $name @ $revision"
        return
    fi

    $GIT clone --recurse-submodules "$url" "$target"
    $GIT -C "$target" checkout --detach "$revision"
    $GIT -C "$target" submodule update --init --recursive
    echo "cloned $name @ $revision"
}

for name in N64Recomp N64ModernRuntime rt64 hlslpp libultraship RecompFrontend; do
    checkout_pinned "$name" "$(require_lock_value "$name.url")" "$(require_lock_value "$name.commit")"
done

echo "All pinned upstream checkouts are ready"
