#!/usr/bin/env python3
"""Recover Knife Edge's overlay layout from the ROM.

The game loads code and data through descriptors that the loaders at
0x800C3848 / 0x800C38C8 / 0x800C3930 / 0x800C3968 consume. All of them agree on
the first four words:

    0x00  dest        destination in RDRAM
    0x04  destEnd     end of the destination region (dest + text + bss)
    0x08  romStart    source in ROM
    0x0C  romEnd      end of source

func_800C3848 additionally invalidates the instruction cache over the loaded
range, which is what distinguishes a code overlay from a data one; that is
evidence about the loader, not about any individual descriptor, so each
descriptor found here is classified from its own contents instead.

Usage:
    overlay_map.py <rom> [--emit-sections] [--emit-splat]
"""
import argparse
import struct
import sys

VRAM_LO, VRAM_HI = 0x80100000, 0x80400000
ENTRY, RESIDENT_END = 0x800C2400, 0x800EEF50
RESIDENT_DATA = (0x27200, 0x2DB50)


def words(rom, off, n):
    return struct.unpack(f">{n}I", rom[off:off + 4 * n])


def scan(rom):
    """Every 4-word window that is shaped like {dest, destEnd, romStart, romEnd}."""
    found = {}
    lo, hi = RESIDENT_DATA
    for off in range(lo, hi - 16, 4):
        dest, dest_end, rom_start, rom_end = words(rom, off, 4)
        if not (VRAM_LO <= dest < VRAM_HI and dest < dest_end < VRAM_HI):
            continue
        if not (0x1000 <= rom_start < len(rom) and rom_start < rom_end <= len(rom)):
            continue
        size = rom_end - rom_start
        if not (0 < size <= 0x400000):
            continue
        # The DMA never writes past the declared destination region.
        if size > dest_end - dest:
            continue
        # A window shifted by one word off a real descriptor usually reproduces
        # an earlier (dest, romStart); keep the first, which is the aligned one.
        found.setdefault((dest, rom_start), (off, dest, dest_end, rom_start, rom_end))
    return sorted(found.values(), key=lambda d: d[3])


def classify(rom, rom_start, rom_end):
    """Code or data, by MIPS function-prologue and `jr $ra` density.

    `addiu $sp, $sp, -N` and `jr $ra` are both rare as incidental byte patterns,
    while `jal`-shaped words are common in compressed art, so jal is not used.
    """
    n = (rom_end - rom_start) // 4
    if n == 0:
        return "empty", 0, 0
    ws = struct.unpack(f">{n}I", rom[rom_start:rom_start + 4 * n])
    prologues = sum(1 for w in ws if (w & 0xFFFF8000) == 0x27BD8000)
    returns = sum(1 for w in ws if w == 0x03E00008)
    kind = "code" if prologues >= 4 and returns >= 4 else "data"
    return kind, prologues, returns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--emit-sections", action="store_true",
                    help="print [[section]] stanzas for the N64Recomp symbol map")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    descs = scan(rom)

    code = [d for d in descs if classify(rom, d[3], d[4])[0] == "code"]

    if args.emit_sections:
        print("# Code overlays recovered by tools/overlay-map/overlay_map.py.")
        print("# Each is relocatable: several share a destination and are swapped at runtime.")
        for i, (off, dest, dest_end, rs, re) in enumerate(code):
            print()
            print("[[section]]")
            print(f'name = "ovl_{rs:06X}"')
            print(f"rom = 0x{rs:X}")
            print(f"vram = 0x{dest:X}")
            print(f"size = 0x{re - rs:X}")
            print("functions = []  # TODO: recover boundaries for this overlay")
        return

    print(f"{len(descs)} descriptors, {len(code)} of them code\n")
    hdr = f"{'tbl':>7}  {'dest':>10} {'destEnd':>10} {'destSz':>8}  {'romStart':>9} {'romEnd':>9} {'romSz':>8}  {'kind':<5} {'prol':>5} {'jrra':>5}"
    print(hdr)
    print("-" * len(hdr))
    for off, dest, dest_end, rs, re in descs:
        kind, prologues, returns = classify(rom, rs, re)
        print(f"{off:7X}  {dest:10X} {dest_end:10X} {dest_end-dest:8X}  "
              f"{rs:9X} {re:9X} {re-rs:8X}  {kind:<5} {prologues:5} {returns:5}")

    print("\nDestinations shared by more than one descriptor (these are swapped at runtime):")
    by_dest = {}
    for off, dest, dest_end, rs, re in descs:
        by_dest.setdefault(dest, []).append((rs, re))
    for dest, srcs in sorted(by_dest.items()):
        if len(srcs) > 1:
            print(f"  0x{dest:08X}  {len(srcs)} sources: " +
                  ", ".join(f"0x{a:X}" for a, _ in srcs))


if __name__ == "__main__":
    sys.exit(main())
