#!/bin/sh
# Bounded, unattended run of the packaged app, reporting only the things that
# say how far boot got.
#
# Run from outside the build tree on purpose: a run inside it can find a
# writable config directory that a real install would not have.
#
#   scripts/smoke_run.sh [vi_frames]
#
# Exit status is the app's. 138 is SIGBUS, 139 SIGSEGV, 134 abort -- for any
# of those, the newest ~/Library/Logs/DiagnosticReports/knife_edge-*.ips names
# the faulting guest function, which is the fastest way to the next boundary.
set -eu
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
require_rom

FRAMES=${1:-600}
APP="$ROOT/Knife Edge Nose Gunner.app/Contents/MacOS/knife_edge"
[ -x "$APP" ] || { echo "No packaged app; run scripts/build_app.sh first" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TRACE="$WORK/trace.csv"

set +e
KNIFE_EDGE_TRACE_OVERLAYS=1 "$APP" \
    --rom "$ROM" --stop-after-vi "$FRAMES" --trace-output "$TRACE" \
    >"$WORK/out.txt" 2>&1
status=$?
set -e

echo "exit=$status"

echo
echo "=== overlay loads (beyond boot neutralization) ==="
grep -i "overlay" "$WORK/out.txt" | grep -v "neutraliz" || echo "  (none)"

echo
echo "=== RSP adapter ==="
grep -i "rsp\|ucode\|task type" "$WORK/out.txt" || echo "  (none)"

echo
echo "=== diagnostics ==="
grep -iE "error|fail|unhandled|assert|stub|unable" "$WORK/out.txt" || echo "  (none)"

echo
echo "=== VI progress ==="
if [ -f "$TRACE" ]; then
    echo "  rows: $(grep -c '' "$TRACE")"
    tail -2 "$TRACE"
else
    echo "  (no trace written -- the game thread died before the first VI)"
fi
exit "$status"
