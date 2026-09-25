#!/usr/bin/env python3
"""Find map functions that fall off their own end.

find_truncated.py and find_unbalanced.py both key on the stack: a function that
allocates a frame and loses its restore. A leaf function allocates nothing, so
both skip it -- and a leaf cut short is just as broken. The recompiled code runs
to the map's end and returns, and everything after the cut is never compiled at
all, because no other map entry covers it.

Knife Edge's keyframe advance, ovl_02FC30_func_8016F7F8, is the example. It
loads a keyframe's angle and times, then divides the angle by the frames left,
at 0x8016FA00, and stores the per-step turn at 0x8016FA5C. The map ended it at
0x8016F9B4. So every scripted object kept the keyframe's whole rotation as its
per-step turn and applied it every frame -- bay doors, wings and the carrier
spinning where the real code swings them once.

This uses control flow rather than the stack. Compiled code never falls from
one function into the next, so a function's extent must (a) end with an
unconditional jump and its delay slot, and (b) contain every branch target
inside it. Where the map's extent fails either, it is grown to the smallest end
that satisfies both.

Usage: find_falloff.py [--fix]   (--fix appends to decomp/size-overrides.txt)
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from find_unbalanced import instructions, map_sizes, ROOT  # noqa: E402

BRANCHES = {
    "b", "beq", "bne", "beqz", "bnez", "blez", "bgtz", "bltz", "bgez",
    "beql", "bnel", "beqzl", "bnezl", "blezl", "bgtzl", "bltzl", "bgezl",
    "bc1t", "bc1f", "bc1tl", "bc1fl",
}
TARGET = re.compile(r"\.L([0-9A-Fa-f]{8})")
LIMIT = 0x10000  # never grow a function by more than this; beyond it, report only
# Instructions N64Recomp rejects outright ("Unhandled instruction"). Compiled
# game code essentially never contains them, so meeting one while growing a
# function means the growth has run into data Splat decoded as code -- as with
# ovl_255660_func_80205DD0, whose tail decodes as `tnei`.
UNHANDLED = {"tnei", "teqi", "tgei", "tgeiu", "tlti", "tltiu", "tne", "teq",
             "tge", "tgeu", "tlt", "tltu", "ll", "sc", "lld", "scd"}  # never grow a function by more than this; beyond it, report only


def is_terminal(mnem, ops):
    """An unconditional transfer that never falls through."""
    if mnem in ("jr", "j", "b"):
        return True
    return mnem == "beq" and ops.startswith("$zero, $zero")


def closed_end(body, index, start, end):
    """Smallest end >= `end` that is closed under branches and ends terminally."""
    while True:
        grown = end
        for vram, _word, mnem, ops in body[index.get(start, 0):]:
            if vram >= grown:
                break
            if mnem in BRANCHES:
                target = TARGET.search(ops)
                if target and int(target.group(1), 16) >= grown:
                    grown = int(target.group(1), 16) + 4
        prev = index.get(grown - 8)
        if grown == end and prev is not None and is_terminal(body[prev][2], body[prev][3]):
            return end
        if grown == end:
            grown += 4  # not yet terminal: take the next instruction
        if grown - start > LIMIT or grown - 4 not in index:
            return None
        # Check everything this step takes in -- including the instruction just
        # added above -- before accepting it.
        for vram, _word, mnem, ops in body[index.get(end, len(body)):]:
            if vram >= grown:
                break
            # Never grow over data: Splat writes it as `.word` with an "invalid
            # instruction" note, or it decodes as something N64Recomp rejects.
            if mnem.startswith(".") or mnem in UNHANDLED:
                return None
            # Nor over a switch dispatch (`jr` through anything but $ra) unless
            # the function's own bounds check comes first: N64Recomp sizes a
            # jump table from that check, and a fragment that starts mid-switch
            # has none -- "Failed to determine size of jump table", as
            # ovl_0EF7C0_func_8017DEA0 showed.
            if mnem == "jr" and not ops.startswith("$ra"):
                bounded = any(m == "sltiu" for v, _w, m, _o in body[index[start]:index[vram]])
                if not bounded:
                    return None
        end = grown


def main():
    sizes = map_sizes(ROOT / "decomp/knife-edge.candidate.syms.toml")
    short, unresolved = [], []
    for path in sorted((ROOT / "asm").glob("*.s")):
        body = instructions(path)
        if not body:
            continue
        index = {vram: i for i, (vram, _w, _m, _o) in enumerate(body)}
        stem = path.stem.upper().rjust(6, "0")
        sections = {s for (s, _) in sizes if s and s.endswith(stem)}
        if path.stem == "1050":
            sections = {s for (s, _) in sizes if s and not s.startswith("ovl_")}
        for section in sections:
            for (sec, vram), size in sizes.items():
                if sec != section or vram not in index:
                    continue
                end = closed_end(body, index, vram, vram + size)
                if end is None:
                    unresolved.append((section, vram, size))
                elif end > vram + size:
                    short.append((section, vram, size, end - vram))

    for section, vram, size, real in sorted(short):
        print(f"  {section:<12} 0x{vram:08X} map 0x{size:X} -> closed 0x{real:X}")
    print(f"\n{len(short)} function(s) fall off their own end")
    if unresolved:
        print(f"{len(unresolved)} could not be closed within 0x{LIMIT:X} bytes (left alone)")

    if "--fix" in sys.argv and short:
        path = ROOT / "decomp/size-overrides.txt"
        with path.open("a") as out:
            out.write("\n# Functions that fell off their own end -- a branch target outside\n"
                      "# the extent, or no terminal jump before it -- found by\n"
                      "# tools/epilogue-audit/find_falloff.py. Sizes are control-flow closed.\n")
            for section, vram, _size, real in sorted(short):
                out.write(f"{section} 0x{vram:08X} 0x{real:X}\n")
        print(f"appended {len(short)} override(s) to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
