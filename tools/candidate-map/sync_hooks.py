#!/usr/bin/env python3
"""Keep config/recomp-candidate.toml's hooks attached after the map changes.

A `[[patches.hook]]` names a function and an address, and it runs only in the
generated copy of that function. Two things break that silently when function
extents change:

- An orphan: the named function was absorbed by a widened neighbour and dropped
  from the map. N64Recomp then refuses the build ("has a function hook but does
  not exist"). It is re-addressed to the smallest function now containing it.
- A missing copy: extents overlap, so the same code is compiled into several
  functions, and a hook in one copy never runs when the game enters another. The
  second menu's wait loop was hooked only in ovl_1199D0_func_8017B0C0's copy
  while the game ran ovl_1199D0_func_8017B650's, and the menu wedged. The hook
  is added to every function in the same section that contains its address.

Run after every map regeneration. Usage: sync_hooks.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MAP = ROOT / "decomp/knife-edge.candidate.syms.toml"
CONFIG = ROOT / "config/recomp-candidate.toml"
HOOK = re.compile(r'func = "(\w+)"\nbefore_vram = (0x[0-9A-Fa-f]+)\ntext = "((?:[^"\\\n]|\\.)*)"')


def section(name):
    return name.split("_func_")[0] if name.startswith("ovl_") else "main"


def main():
    entries = []
    for line in MAP.read_text().splitlines():
        found = re.search(r'name = "(\w+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', line)
        if found:
            entries.append((found.group(1), int(found.group(2), 16), int(found.group(3), 16)))
    names = {name for name, _, _ in entries}

    def containing(sec, vram):
        return [(name, size) for name, start, size in entries
                if section(name) == sec and start <= vram < start + size]

    text = CONFIG.read_text()
    readdressed = []

    def readdress(match):
        func, vram = match.group(1), int(match.group(2), 16)
        if func in names:
            return match.group(0)
        owners = containing(section(func), vram)
        if not owners:
            return match.group(0)
        owner = min(owners, key=lambda o: o[1])[0]
        readdressed.append((func, owner, vram))
        return match.group(0).replace(f'func = "{func}"', f'func = "{owner}"', 1)

    text = HOOK.sub(readdress, text)

    hooks = [(m.group(1), int(m.group(2), 16), m.group(3)) for m in HOOK.finditer(text)]
    have = {(func, vram) for func, vram, _ in hooks}
    copies = []
    for func, vram, body in hooks:
        for name, _size in containing(section(func), vram):
            if (name, vram) not in have:
                have.add((name, vram))
                copies.append((name, vram, body))
    if copies:
        text = text.rstrip() + "\n\n# Copies of existing hooks for overlapping functions (sync_hooks.py).\n"
        text += "".join(f'[[patches.hook]]\nfunc = "{name}"\nbefore_vram = 0x{vram:08X}\n'
                        f'text = "{body}"\n\n' for name, vram, body in copies)
    CONFIG.write_text(text)

    orphans = sorted({m.group(1) for m in HOOK.finditer(text)} - names)
    print(f"{len(readdressed)} re-addressed, {len(copies)} copied into overlapping functions, "
          f"{len(orphans)} still orphaned")
    for func, owner, vram in readdressed:
        print(f"  0x{vram:08X}: {func} -> {owner}")
    for func in orphans:
        print(f"  ORPHAN {func}")
    return 1 if orphans else 0


if __name__ == "__main__":
    sys.exit(main())
