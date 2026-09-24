#!/usr/bin/env python3
"""Find map functions that end before their stack ever comes back to zero.

find_truncated.py compared the map's extents against Splat's. That catches a map
that cuts a function short, but not the worse case: a phantom entry point. When
the probe invents a function start in the middle of a real one, Splat adopts the
same boundary, map and Splat agree, and both are wrong. `func_8017B48C` is the
example -- it allocates 0x38, and the `addiu $sp, $sp, 0x38` that gives it back
lives in what the map calls `func_8017B650`, so every call leaked 0x38.

So this trusts neither. It walks the instruction stream from each function's
prologue, tracks the stack pointer, and takes the function's real end to be the
last `jr $ra` reached with the stack balanced. Extending stops at the next map entry
that something calls *and* that sits on a balanced stack. Both halves matter:
eight overlays share the VRAM at 0x8017B0C0, so a jal to an address proves only
that it is a function in some overlay, not in this one. An entry reached while
the stack is still down cannot be a function start here whoever calls it --
`func_8017B650` opens with `swc1 $ft4, 0x28($sp)`, a store into a frame it never
allocated.

Usage: find_unbalanced.py [--fix]  (--fix appends to decomp/size-overrides.txt)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
INSTRUCTION = re.compile(
    r"/\* [0-9A-Fa-f]+ ([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8}) \*/\s+(\S+)\s*(.*)")
ALLOC = re.compile(r"\$sp, \$sp, (-?)0x([0-9A-Fa-f]+)")


def instructions(path):
    """Every instruction in the file, in address order, boundaries ignored."""
    out = []
    for line in path.read_text().splitlines():
        found = INSTRUCTION.search(line)
        if found:
            out.append((int(found.group(1), 16), int(found.group(2), 16),
                        found.group(3), found.group(4)))
    return out


def map_sizes(path):
    section, out = None, {}
    for line in path.read_text().splitlines():
        header = re.search(r'name\s*=\s*"(ovl_[0-9A-F]+|\w+)"\s*$', line)
        if header and "vram" not in line:
            section = header.group(1)
        entry = re.search(r'name = "(\w+)", vram = (0x[0-9A-Fa-f]+), '
                          r'size = (0x[0-9A-Fa-f]+)', line)
        if entry:
            out[(section, int(entry.group(2), 16))] = int(entry.group(3), 16)
    return out


def call_targets(files):
    """Every address reached by a jal, decoded from the instruction words."""
    out = set()
    for body in files.values():
        for _vram, word, _m, _o in body:
            if word >> 26 == 0x03:  # jal
                out.add(0x80000000 | ((word & 0x03FFFFFF) << 2))
    return out


def real_end(body, start, entries, called):
    """Last balanced `jr $ra` from the prologue at index `start`, or None."""
    delta, end = 0, None
    for i in range(start, len(body)):
        vram, _word, mnem, ops = body[i]
        if i > start and delta == 0 and vram in entries and vram in called:
            break  # a real function: balanced stack, and something calls it
        if mnem == "addiu":
            found = ALLOC.match(ops)
            if found:
                step = int(found.group(2), 16)
                delta += -step if found.group(1) else step
        if mnem == "jr" and ops.startswith("$ra") and delta == 0 and i + 1 < len(body):
            end = body[i + 1][0] + 4  # past the delay slot
    return end


def main():
    sizes = map_sizes(ROOT / "decomp/knife-edge.candidate.syms.toml")
    files = {p: instructions(p) for p in sorted((ROOT / "asm").glob("*.s"))}
    called = call_targets(files)
    short = []
    for path, body in files.items():
        stem = path.stem.upper().rjust(6, "0")
        sections = {s for (s, _) in sizes if s and s.endswith(stem)}
        if path.stem == "1050":
            sections = {s for (s, _) in sizes if s and not s.startswith("ovl_")}
        if not sections:
            continue
        index = {vram: i for i, (vram, _w, _m, _o) in enumerate(body)}
        for section in sections:
            entries = {v for (s, v) in sizes if s == section}
            for vram in sorted(entries):
                i = index.get(vram)
                if i is None or body[i][2] != "addiu":
                    continue
                found = ALLOC.match(body[i][3])
                if not found or not found.group(1):
                    continue  # no frame allocated: nothing to leak
                size = sizes[(section, vram)]
                end = real_end(body, i, entries, called)
                if end is not None and end > vram + size:
                    short.append((section, vram, size, end - vram))

    for section, vram, size, real in sorted(short):
        print(f"  {section:<12} 0x{vram:08X} map 0x{size:X} -> real 0x{real:X}")
    print(f"\n{len(short)} function(s) end before their stack is balanced")

    if "--fix" in sys.argv and short:
        path = ROOT / "decomp/size-overrides.txt"
        with path.open("a") as out:
            out.write("\n# Functions whose map extent ends while the stack is still\n"
                      "# down, found by tools/epilogue-audit/find_unbalanced.py.\n"
                      "# Sizes run to the last `jr $ra` reached with sp balanced.\n")
            for section, vram, _size, real in sorted(short):
                out.write(f"{section} 0x{vram:08X} 0x{real:X}\n")
        print(f"appended {len(short)} override(s) to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
