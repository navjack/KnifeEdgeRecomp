#!/usr/bin/env python3
"""Find map functions whose extent cuts off their own stack restore.

A recompiled function that allocates a frame with `addiu $sp, $sp, -N` and is
then cut short before the matching `addiu $sp, $sp, N` leaks that much stack on
every call, permanently: $sp is an ordinary register in the generated C, so
nothing ever puts it back. The caller keeps running with a displaced frame and
every subsequent `lw $x, off($sp)` reads the wrong word.

That failure is silent. It does not crash, it does not warn, and the symptom
turns up arbitrarily far away -- in Knife Edge as a title loop that waited
forever on a flag, because the thread that should have set it was reading its
message from a stack slot that had moved.

Splat knows where each function really ends (glabel .. endlabel). This compares
the map's extents against that and reports every function the map truncates
past its prologue, with the size it should have had.

Usage: find_truncated.py [--fix]   (--fix appends to decomp/size-overrides.txt)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
INSTRUCTION = re.compile(
    r"/\* [0-9A-Fa-f]+ ([0-9A-Fa-f]{8}) [0-9A-Fa-f]{8} \*/\s+(\S+)\s*(.*)")


def splat_functions(path):
    """(name, vram, [(vram, mnemonic, operands)]) for each glabel..endlabel."""
    name, body = None, []
    for line in path.read_text().splitlines():
        start = re.match(r"glabel (\w+)", line)
        if start:
            name, body = start.group(1), []
            continue
        if line.startswith("endlabel") and name is not None:
            if body:
                yield name, body[0][0], body
            name, body = None, []
            continue
        found = INSTRUCTION.search(line)
        if found and name is not None:
            body.append((int(found.group(1), 16), found.group(2), found.group(3)))


def map_sizes(path):
    """(section, vram) -> size, from the generated symbol map."""
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


def main():
    sizes = map_sizes(ROOT / "decomp/knife-edge.candidate.syms.toml")
    # asm/3D600.s holds what the map calls ovl_03D600; asm/1050.s is resident.
    truncated = []
    for path in sorted((ROOT / "asm").glob("*.s")):
        stem = path.stem.upper().rjust(6, "0")
        sections = {s for (s, _) in sizes if s and s.endswith(stem)}
        if path.stem == "1050":
            sections = {s for (s, _) in sizes if s and not s.startswith("ovl_")}
        for name, vram, body in splat_functions(path):
            allocates = any(
                m == "addiu" and re.match(r"\$sp, \$sp, -0x", o) for _, m, o in body)
            if not allocates:
                continue
            real_size = body[-1][0] + 4 - vram
            for section in sections:
                size = sizes.get((section, vram))
                if size is None or size >= real_size:
                    continue
                covered = [b for b in body if b[0] < vram + size]
                restores = any(
                    m == "addiu" and re.match(r"\$sp, \$sp, 0x", o) for _, m, o in covered)
                if not restores:
                    truncated.append((section, name, vram, size, real_size))

    for section, name, vram, size, real in sorted(truncated):
        print(f"  {section:<12} {name:<24} 0x{vram:08X} "
              f"map 0x{size:X} -> real 0x{real:X}")
    print(f"\n{len(truncated)} function(s) cut off before their stack restore")

    if "--fix" in sys.argv and truncated:
        path = ROOT / "decomp/size-overrides.txt"
        with path.open("a") as out:
            out.write("\n# Extents that cut a function off before its own\n"
                      "# `addiu $sp, $sp, N`, found by tools/epilogue-audit.\n"
                      "# Sizes are Splat's own glabel..endlabel extents.\n")
            for section, _name, vram, _size, real in sorted(truncated):
                out.write(f"{section} 0x{vram:08X} 0x{real:X}\n")
        print(f"appended {len(truncated)} override(s) to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
