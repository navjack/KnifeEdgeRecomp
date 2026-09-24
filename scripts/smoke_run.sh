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

# Guest words worth a column in every run, all established by reading the
# boot path out of asm/1050.s:
#   8013C280  graphics tasks the game has queued and not yet seen finish.
#             func_800D1BB4 increments it, func_800D2B40 busy-waits for it to
#             reach zero, and the two task-done threads decrement it. A value
#             that goes up once and stays there is the whole stall.
#   801423F4  validCount of the scheduler queue func_800D1BB4 posts tasks to
#   801423FC  msgCount of the same queue (8 if osCreateMesgQueue ran)
#   80128A60  the deferred-work function pointer the boot thread spins on
WATCH="${WATCH:-8013C280 801423F4 801423FC 80128A60}"
watch_args=""
for address in $WATCH; do
    watch_args="$watch_args --watch $address"
done

set +e
# shellcheck disable=SC2086
KNIFE_EDGE_TRACE_OVERLAYS=1 "$APP" \
    --rom "$ROM" --stop-after-vi "$FRAMES" --trace-output "$TRACE" $watch_args \
    >"$WORK/out.txt" 2>&1
status=$?
set -e

# A SIGSEGV whose trace reached the requested frame count is the teardown
# race, not a guest fault: ultramodern::quit() does not join the guest threads,
# so main unmaps RDRAM out from under whichever one is still running. Say so,
# rather than reporting a clean run as a crash.
if [ "$status" -eq 139 ] && [ -f "$TRACE" ] && \
   [ "$(tail -1 "$TRACE" | cut -d, -f1)" = "$FRAMES" ]; then
    echo "exit=$status (teardown race after the full $FRAMES frames, not a guest fault)"
else
    echo "exit=$status"
fi

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
    head -1 "$TRACE"
    sed -n '2p;3p' "$TRACE"
    echo "  ..."
    tail -2 "$TRACE"
else
    echo "  (no trace written -- the game thread died before the first VI)"
fi
exit "$status"
