#!/usr/bin/env python3
"""Fit the pacing model's three hardware costs to real-hardware footage.

Inputs:
  intro.csv  KNIFE_EDGE_RDP_MODEL_CSV from an untouched boot through the story
             (KNIFE_EDGE_RDP_MODEL=measure KNIFE_EDGE_RDP_VIS=1, no input).
  play.csv   the same through stage 1's two transmissions, health pinned.
  hw.txt     tools/pacing-fit/extract_hw.sh output for the longplay.

Each port frame's cost is recomputed from its raw counts for a candidate
(CPI, RDP bandwidth, span cost), rounded up to whole retraces, and summed over
the title, the story and 14 windows of 300 gameplay steps; the fit minimises
the RMS relative error of those durations against the footage. The VI's
framebuffer scanout is taken out of the RDP's bandwidth at each frame's
resolution, as in native/game/rdp_model.cpp.

Usage: fit.py intro.csv play.csv hw.txt [play_first_gfx]
"""
import csv
import json
import math
import pathlib
import re
import sys

VI = 1000 / 59.94
RCP_PER_MS = 62.5e3
MARKERS = json.loads((pathlib.Path(__file__).parent / "longplay_markers.json").read_text())


def load(path):
    return {int(r["gfx"]): r for r in csv.DictReader(open(path))}


def step_ms(r, cpi, bw, span):
    f = lambda k: float(r[k])
    hires = f("px_fill") >= 900000  # the title: three 640x480 fills
    bw = bw - (0.59 if hires else 0.147)
    if bw <= 0.05:
        return 1e9
    cpu = f("step_instructions") * cpi / 93750.0
    rdp = (max(f("pipe_cycles"), f("pixel_bytes") / bw) + f("texture_bytes") / bw
           + f("spans") * span + f("tris_drawn") * 30 + f("sync_cycles"))
    return max(cpu, f("host_ms")) + max(f("rsp_ms"), rdp / RCP_PER_MS)


def duration(rows, p):
    return sum(max(1, math.ceil(step_ms(r, *p) / VI)) for r in rows) * VI / 1000


def main():
    intro, play, hw_path = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
    targets = []
    for name in ("title", "story"):
        m = MARKERS[name]
        lo, hi = m["port_gfx"]
        targets.append((name, [intro[g] for g in range(lo, hi) if g in intro], m["end"] - m["start"]))

    stretch = MARKERS["stage1_between_transmissions"]
    pauses = stretch["pauses"]
    game_time = lambda t: t - sum(min(max(t - a, 0), b - a) for a, b in pauses)
    diffs = [float(x) for x in re.findall(r"YAVG=([\d.]+)", open(hw_path).read())]
    hw = []
    for i, d in enumerate(diffs):
        t = 254.0 + (i + 1) / 30
        if stretch["start"] <= t < stretch["end"] and not any(a <= t < b for a, b in pauses) and d >= 0.3:
            hw.append(game_time(t))
    first = int(sys.argv[4]) if len(sys.argv) > 4 else min(play)
    steps = [play[g] for g in range(first, first + stretch["port_steps"]) if g in play]
    scale = len(hw) / len(steps)
    at = lambda k: (game_time(stretch["start"]) if k * scale <= 0 else
                    game_time(stretch["end"]) if k * scale >= len(hw) else hw[int(k * scale)])
    for k in range(0, len(steps) - 299, 300):
        targets.append((f"play {k:4d}", steps[k:k + 300], at(k + 300) - at(k)))

    def score(p):
        return math.sqrt(sum(((duration(rows, p) - t) / t) ** 2 for _, rows, t in targets) / len(targets))

    best = min((score((c, b, s)), (c, b, s)) for c in (2, 3, 4, 5, 6, 7, 8)
               for b in (0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.4, 3.0) for s in (0, 4, 8, 16, 32))
    _, (c0, b0, s0) = best
    best = min([best] + [(score((c0 + dc / 4, b0 * (1 + db / 20), max(0, s0 + ds))), (c0 + dc / 4, b0 * (1 + db / 20), max(0, s0 + ds)))
                         for dc in range(-4, 5) for db in range(-4, 5) for ds in range(-4, 5, 2)])
    err, p = best
    print(f"CPI {p[0]:.2f}, raw RDP bandwidth {p[1]:.2f} B/cycle ({p[1] * 62.5:.0f} MB/s), "
          f"span {p[2]} cycles: {100 * err:.1f}% RMS over {len(targets)} targets")
    for name, rows, t in targets:
        d = duration(rows, p)
        print(f"  {name:10s} {len(rows):5d} steps  hardware {t:7.2f} s  model {d:7.2f} s  ({100 * (d - t) / t:+.1f}%)")


if __name__ == "__main__":
    main()
