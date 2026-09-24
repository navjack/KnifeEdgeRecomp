#!/usr/bin/env python3
"""Check the overlay bridge's unload rule against the ROM's real descriptors.

librecomp's unload_overlays() exits the process if the range it is given does
not fully contain a section it overlaps, and that only happens once the game is
running, deep into a level transition. This replays the same widen-then-unload
rule the C++ in native/game/overlay_bridge.cpp implements, over the actual
descriptor table, and fails if any sequence would hit that check.

Run it after changing either the bridge or the overlay table.
"""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def code_overlays():
    spec = importlib.util.spec_from_file_location(
        "overlay_map", ROOT / "tools/overlay-map/overlay_map.py")
    om = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(om)
    rom = (ROOT / "rom/Knife Edge - Nose Gunner (U) [!].z64").read_bytes()
    return {d[3]: (d[1], d[4] - d[3]) for d in om.scan(rom)
            if om.classify(rom, d[3], d[4])[0] == "code"}, rom


def bridge(loaded, rom_start, dest, size, code):
    """Mirror of knife_edge_overlay_dma()."""
    lo, hi, overlaps = dest, dest + size, False
    changed = True
    while changed:
        changed = False
        for addr, extent in loaded.items():
            if addr >= hi or addr + extent <= lo:
                continue
            overlaps = True
            if addr < lo:
                lo, changed = addr, True
            if addr + extent > hi:
                hi, changed = addr + extent, True
    unload = (lo, hi) if overlaps else None
    if overlaps:
        for addr in [a for a in loaded if a >= lo and a + loaded[a] <= hi]:
            del loaded[addr]
    if rom_start in code and code[rom_start][1] <= size:
        loaded[dest] = code[rom_start][1]
    return unload


def main():
    code, _rom = code_overlays()
    order = sorted(code)
    # Every overlay once, then every adjacent pair both ways -- enough to cover
    # a small module replacing a large one at a shared destination, which is the
    # case that trips the partial-unload check.
    sequences = [order]
    for a in order:
        for b in order:
            if a != b:
                sequences.append([a, b, a])

    fatals = 0
    for seq in sequences:
        loaded = {}
        for rom_start in seq:
            dest, size = code[rom_start]
            snapshot = dict(loaded)
            unload = bridge(loaded, rom_start, dest, size, code)
            if not unload:
                continue
            lo, hi = unload
            for addr, extent in snapshot.items():
                overlapping = addr < hi and addr + extent > lo
                contained = addr >= lo and addr + extent <= hi
                if overlapping and not contained:
                    print(f"FATAL: unload 0x{lo:08X}..0x{hi:08X} would partially "
                          f"unload 0x{addr:08X}+0x{extent:X} "
                          f"(loading rom 0x{rom_start:X})")
                    fatals += 1

    print(f"{len(code)} code overlays, {len(sequences)} sequences, "
          f"{fatals} partial-unload fatals")
    return 1 if fatals else 0


if __name__ == "__main__":
    sys.exit(main())
