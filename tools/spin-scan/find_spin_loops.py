#!/usr/bin/env python3
"""Find guest loops that can never let the runtime deliver a message.

ultramodern moves SP-done, DP-done and VI-retrace messages out of its external
queue and into the game's own message queues only at the *start* of an os*
call, and it schedules guest threads one at a time, the way the N64 did. A loop
that calls nothing therefore does two fatal things at once: it never drains the
queue, and it never gives up the CPU. On hardware neither mattered, because an
interrupt ran the handler regardless.

So the loops that matter are exactly the ones with no call in the body. This
prints them, with the address of the backward branch itself as the suggested
hook point -- deliberately the branch and not its target, because the target is
a label and a hook placed there can land outside the loop.

Usage: find_spin_loops.py [asm-dir]
"""
import pathlib
import re
import sys

INSTRUCTION = re.compile(
    r"/\* [0-9A-Fa-f]+ ([0-9A-Fa-f]{8}) [0-9A-Fa-f]{8} \*/\s+(\S+)\s*(.*)")
LABEL = re.compile(r"^\s*\.?(?:L|alabel )([0-9A-Fa-f]{8}|\w+)")
GLABEL = re.compile(r"^glabel (\w+)")
# Branches whose target is an address in the same function.
BRANCH = re.compile(r"^b(?:eq|ne|gez|gtz|lez|ltz|eql|nel|c1[tf])?[lz]?$|^b$")
CALL = ("jal", "jalr", "syscall")


def scan(path):
    """Yield (function, branch_vram, target_vram, body_size, globals) per wait."""
    function = None
    instructions = []          # (vram, mnemonic, operands) in order
    for line in path.read_text().splitlines():
        glabel = GLABEL.match(line)
        if glabel:
            yield from loops(path, function, instructions)
            function, instructions = glabel.group(1), []
            continue
        found = INSTRUCTION.search(line)
        if found:
            vram = int(found.group(1), 16)
            instructions.append((vram, found.group(2), found.group(3)))
    yield from loops(path, function, instructions)


def loops(path, function, instructions):
    if function is None:
        return
    by_vram = {vram: i for i, (vram, _, _) in enumerate(instructions)}
    for index, (vram, mnemonic, operands) in enumerate(instructions):
        if not BRANCH.match(mnemonic.rstrip("l") if mnemonic != "b" else "b"):
            continue
        target = re.search(r"\.L([0-9A-Fa-f]{8})", operands)
        if not target:
            continue
        target_vram = int(target.group(1), 16)
        if target_vram >= vram:      # only backward branches close a loop
            continue
        start = by_vram.get(target_vram)
        if start is None:
            continue
        body = instructions[start:index + 2]   # include the delay slot
        if any(m in CALL for _, m, _ in body):
            continue
        # A wait re-reads one fixed global until something else changes it, so
        # it is short, it loads through an absolute %lo(D_...) address, and it
        # never stores. Without all three the scan drowns in ordinary work
        # loops -- memcpy, decompression, table walks -- which read memory
        # every iteration too but are not waiting on anything.
        if len(body) > 12:
            continue
        if any(m.startswith("s") and not m.startswith(("sll", "sra", "srl",
                                                       "slt", "sub", "syscall"))
               for _, m, _ in body):
            continue
        watched = {
            re.search(r"%lo\((\w+)\)", operands).group(1)
            for _, m, operands in body
            if m.startswith(("lw", "lh", "lb")) and "%lo(" in operands
        }
        if not watched:
            continue
        yield function, vram, target_vram, len(body), sorted(watched)


def map_functions(path):
    """(section, name, vram, size) for every function in the candidate map."""
    section = None
    out = []
    for line in path.read_text().splitlines():
        header = re.search(r'name\s*=\s*"(ovl_[0-9A-F]+|\.?\w+)"\s*$', line)
        if header and "vram" not in line:
            section = header.group(1)
        entry = re.search(r'name = "(\w+)", vram = (0x[0-9A-Fa-f]+), '
                          r'size = (0x[0-9A-Fa-f]+)', line)
        if entry:
            out.append((section, entry.group(1),
                        int(entry.group(2), 16), int(entry.group(3), 16)))
    return out


def emit_hooks(root, map_path):
    """Print [[patches.hook]] entries, naming functions as the map does.

    The same VRAM belongs to as many sections as share that overlay slot, and a
    hook has to name one exactly, so the names come from the map rather than
    from the disassembly -- and a loop whose function the map does not have is
    skipped rather than guessed at, because N64Recomp hard-errors on a hook
    naming a function that does not exist.
    """
    functions = map_functions(map_path)
    seen = set()
    for path in sorted(root.glob("*.s")):
        # asm/3D600.s holds the section the map calls ovl_03D600.
        stem = path.stem.upper().rjust(6, "0")
        for _, branch, _, _, watched in scan(path):
            if any(w.startswith("D_A4") for w in watched):
                continue  # hardware polls, in libultra code the port replaces
            match = [
                (sec, name) for sec, name, vram, size in functions
                if vram <= branch < vram + size
                and (sec is None or sec.endswith(stem) or path.stem == "1050")
            ]
            if not match:
                continue
            _, name = match[0]
            if (name, branch) in seen:
                continue
            seen.add((name, branch))
            print(f"\n[[patches.hook]]")
            print(f"# waits on {' '.join(watched)}")
            print(f'func = "{name}"')
            print(f"before_vram = 0x{branch:08X}")
            print('text = "knife_edge_guest_poll(rdram, ctx);"')


def main():
    root = pathlib.Path("asm")
    if "--hooks" in sys.argv:
        emit_hooks(root, pathlib.Path("decomp/knife-edge.candidate.syms.toml"))
        return 0
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "asm")
    total = 0
    for path in sorted(root.glob("*.s")):
        for function, branch, target, size, watched in scan(path):
            total += 1
            print(f"  {path.name:<10} {function:<22} "
                  f"loop 0x{target:08X}..0x{branch:08X} "
                  f"({size:2d} instrs) waits on {' '.join(watched)}")
    print(f"\n{total} callless wait loop(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
