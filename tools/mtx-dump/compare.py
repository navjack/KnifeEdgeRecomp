#!/usr/bin/env python3
"""Compare object rotations between two frames of a KNIFE_EDGE_DUMP_MTX log.

The game draws each object as PROJ LOAD (perspective), PROJ MUL (camera), MODL
LOAD (the object), so the Nth MODL LOAD in a frame is the Nth object drawn. This
pairs objects by that draw order and reports how far each one's rotation moved,
per frame, alongside the camera's.

Draw order alone is an assumption about identity, so the Mtx addresses are
printed too. The game double-buffers its display lists (0x8010BAF0 and
0x8010FAF0 alternate), so frames N and N+2 share a buffer, and an object drawn at
the same position in the sequence lands at the same address. Compare N with N+2
and a matching address confirms it is the same object; divide by 2 is applied.

Usage: compare.py LOG A B [A B ...]
"""
import math
import re
import sys


def parse(path):
    """task index -> [(kind, op, address, rows)] in draw order."""
    tasks, current, pending = {}, None, None
    for line in open(path):
        task = re.match(r"\[mtx\] gfx task (\d+)", line)
        if task:
            current = int(task.group(1))
            tasks[current] = []
            continue
        head = re.search(r"#\d+ @0x(\w+) (\w+) (\w+)", line)
        if head and current is not None:
            pending = (head.group(2), head.group(3), int(head.group(1), 16), [])
            tasks[current].append(pending)
            continue
        row = re.match(r"\[mtx\]\s+([+-][\d.]+)\s+([+-][\d.]+)\s+([+-][\d.]+)\s+([+-][\d.]+)", line)
        if row and pending is not None:
            pending[3].append([float(x) for x in row.groups()])
    return tasks


def rotation(rows):
    """The upper 3x3 with the uniform scale divided out."""
    basis = [row[:3] for row in rows[:3]]
    scale = math.sqrt(sum(x * x for x in basis[0])) or 1.0
    return [[x / scale for x in row] for row in basis]


def angle_between(a, b):
    """Degrees of rotation taking one orientation to the other."""
    ra, rb = rotation(a), rotation(b)
    trace = sum(ra[i][k] * rb[i][k] for i in range(3) for k in range(3))
    return math.degrees(math.acos(max(-1.0, min(1.0, (trace - 1.0) / 2.0))))


def select(entries, kind, op):
    return [e for e in entries if e[0] == kind and e[1] == op]


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2 != 0:
        print(__doc__)
        return 2
    tasks = parse(sys.argv[1])
    worst = 0.0
    for a, b in zip(sys.argv[2::2], sys.argv[3::2]):
        a, b = int(a), int(b)
        if a not in tasks or b not in tasks:
            print(f"frames {a}/{b}: not both in the log")
            continue
        frames = b - a
        cameras = list(zip(select(tasks[a], "PROJ", "MUL"), select(tasks[b], "PROJ", "MUL")))
        objects = list(zip(select(tasks[a], "MODL", "LOAD"), select(tasks[b], "MODL", "LOAD")))
        camera = max((angle_between(x[3], y[3]) for x, y in cameras), default=0.0) / frames
        print(f"frames {a} -> {b}: camera {camera:5.1f} deg/frame")
        for index, (x, y) in enumerate(objects):
            step = angle_between(x[3], y[3]) / frames
            same = "same address" if x[2] == y[2] else f"0x{x[2]:06X} vs 0x{y[2]:06X}"
            worst = max(worst, step)
            print(f"  object {index}: {step:6.1f} deg/frame  ({same})")
    print(f"\nlargest object rotation: {worst:.1f} deg/frame")
    return 0


if __name__ == "__main__":
    sys.exit(main())
