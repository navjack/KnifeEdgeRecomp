#!/usr/bin/env python3
"""Drive the candidate map to a clean recompile using N64Recomp's diagnostics.

Splat's function boundaries are wrong in ways that only the recompiler can see,
and each way it complains carries the exact extent it needs:

  "Unhandled branch in F at S to T"
      F must reach T. A `.L` target is intra-function by construction, so the
      boundary is short.
  "Failed to determine size of jump table at J for instruction at I"
      F contains a switch, and N64Recomp only accepts table entries that land
      inside the function, so F must cover every case. The table is read from
      the ROM to find the last one.

Fixing these by hand is whack-a-mole; each fix uncovers the next. This runs the
loop, appending to decomp/size-overrides.txt, until the recompile is clean or
nothing further can be learned.
"""
import pathlib
import re
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
OVERRIDES = ROOT / "decomp/size-overrides.txt"
ROM = ROOT / "rom/Knife Edge - Nose Gunner (U) [!].z64"
MAP = ROOT / "decomp/knife-edge.candidate.syms.toml"

BRANCH_RE = re.compile(r"Unhandled branch in (\S+) at 0x([0-9A-F]+) to 0x([0-9A-F]+)")
JTBL_RE = re.compile(r"Failed to determine size of jump table at 0x([0-9A-F]+) "
                     r"for instruction at 0x([0-9A-F]+)")
ANALYZE_RE = re.compile(r"Failed to analyze (\S+)")
FAILED_RE = re.compile(r"Error in recompiling (\S+), clearing output file")
FUNC_RE = re.compile(r"^(?:(\w+?)_)?func_([0-9A-F]{8})$")


def sections():
    """(name, rom, vram, size) for each section in the current map."""
    out = []
    for m in re.finditer(r'name = "(\w+)"\nrom = 0x([0-9A-F]+)\n'
                         r'vram = 0x([0-9A-F]+)\nsize = 0x([0-9A-F]+)', MAP.read_text()):
        out.append((m[1], int(m[2], 16), int(m[3], 16), int(m[4], 16)))
    return out


def jump_table_end(rom_bytes, table_vram, want_section=None):
    """Highest target in the table, or None if it does not look like one."""
    for name, sec_rom, sec_vram, sec_size in sections():
        if want_section is not None and name != want_section:
            continue
        if not (sec_vram <= table_vram < sec_vram + sec_size):
            continue
        off = sec_rom + (table_vram - sec_vram)
        targets = []
        for i in range(0, 256):
            chunk = rom_bytes[off + i * 4: off + i * 4 + 4]
            if len(chunk) < 4:
                break
            word = struct.unpack(">I", chunk)[0]
            if not (sec_vram <= word < sec_vram + sec_size):
                break
            targets.append(word)
        if targets:
            return max(targets)
    return None


def splat_size(section, vram):
    """Splat's own declared size for a function, used as the anchor when a
    widened extent has clearly run off the end of the code."""
    import re as _re
    for name, sec_rom, _sec_vram, _sec_size in sections():
        if name != section:
            continue
        src = ROOT / "asm" / f"{sec_rom:X}.s"
        if not src.exists():
            return None
        m = _re.search(rf"nonmatching func_{vram:08X}, (0x[0-9A-Fa-f]+)", src.read_text())
        return int(m.group(1), 16) if m else None
    return None


def section_bounds(section):
    for name, _rom, vram, size in sections():
        if name == section:
            return vram, vram + size
    return None, None


def callers_of(vram):
    """How many `jal` sites target this address, across the whole image."""
    needle = f"jal        func_{vram:08X}"
    total = 0
    for src in (ROOT / "asm").glob("*.s"):
        total += src.read_text().count(needle)
    return total


def current_overrides():
    out = {}
    for line in OVERRIDES.read_text().splitlines():
        body = line.split("#", 1)[0].strip()
        if body:
            sec, v, sz = body.split()
            out[(sec, int(v, 16))] = int(sz, 16)
    return out


def run(cmd, cwd=ROOT):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    rom_bytes = ROM.read_bytes()

    for attempt in range(1, rounds + 1):
        gen = run(["./scripts/generate_candidate_symbols.sh"])
        if gen.returncode != 0:
            print(gen.stdout + gen.stderr)
            return 2
        probe = run(["../build/N64Recomp/N64Recomp", "recomp-candidate.toml"],
                    cwd=ROOT / "config")
        text = probe.stdout + probe.stderr
        if "Error recompiling" not in text:
            print(f"round {attempt}: clean")
            print(gen.stdout.strip())
            return 0

        added = []
        for name, _src, dst in BRANCH_RE.findall(text):
            fm = FUNC_RE.match(name)
            if not fm:
                continue
            section = fm.group(1) or "main"
            vram = int(fm.group(2), 16)
            target = int(dst, 16)
            lo, hi = section_bounds(section)
            if lo is None or not (lo <= target < hi):
                # Not a short boundary: the function already extends past the
                # real code and a data word is being decoded as a branch. Pull
                # it back to the size Splat actually declared.
                anchor = splat_size(section, vram)
                already = current_overrides().get((section, vram))
                if anchor and already != anchor:
                    added.append((section, vram, anchor,
                                  f"branch to 0x{dst} is outside the section; "
                                  f"extent too long, reset to Splat's size"))
                else:
                    # Splat's own extent still contains the impossible branch,
                    # so this is not code at all. Drop it.
                    added.append((section, vram, 0,
                                  f"not code: branch to 0x{dst} is impossible "
                                  f"even at Splat's size"))
                continue
            need = target + 4 - vram
            if need > 0:
                added.append((section, vram, need,
                              f"branch to 0x{dst} must be inside it"))

        # N64Recomp names the function it could not analyse on its own line,
        # which identifies both the section and the entry address exactly --
        # far better than guessing an owner from the instruction address, since
        # the same vram exists in several overlays.
        failed = ANALYZE_RE.findall(text)
        tables = JTBL_RE.findall(text)
        for name, (table, _instr) in zip(failed, tables):
            fm = FUNC_RE.match(name)
            if not fm:
                continue
            section = fm.group(1) or "main"
            vram = int(fm.group(2), 16)
            end = jump_table_end(rom_bytes, int(table, 16), section)
            if end is None or end < vram:
                continue
            added.append((section, vram, end + 8 - vram,
                          f"jump table 0x{table} runs to 0x{end:08X}"))

        # Catch-all. Anything else N64Recomp rejects -- an unsupported
        # instruction, a malformed operand -- in a function nothing calls is
        # Splat having labelled data as code. func_801FA54C is one: 8 KB around
        # it holds zero function prologues and zero `jr $ra`.
        if not added:
            handled = {v for _, v, _, _ in added}
            for name in FAILED_RE.findall(text):
                fm = FUNC_RE.match(name)
                if not fm:
                    continue
                section = fm.group(1) or "main"
                vram = int(fm.group(2), 16)
                if vram in handled or callers_of(vram) != 0:
                    continue
                added.append((section, vram, 0,
                              "not code: nothing calls it and it does not "
                              "recompile"))

        if not added:
            print(f"round {attempt}: stuck, nothing actionable in:")
            print("\n".join(l for l in text.splitlines()
                            if not l.startswith("[Info]"))[:2000])
            return 1

        current = {}
        for line in OVERRIDES.read_text().splitlines():
            body = line.split("#", 1)[0].strip()
            if body:
                sec, v, sz = body.split()
                current[(sec, int(v, 16))] = int(sz, 16)

        notes = []
        for section, vram, size, why in added:
            if current.get((section, vram), -1) == size:
                print(f"round {attempt}: {section} 0x{vram:08X} already 0x{size:X}; stuck")
                return 1
            current[(section, vram)] = size
            notes.append(f"{section} 0x{vram:08X} 0x{size:X}  # {why}")
            print(f"round {attempt}: {section} 0x{vram:08X} -> 0x{size:X}  ({why})")

        header = [l for l in OVERRIDES.read_text().splitlines()
                  if l.startswith("#") or not l.strip()]
        body = [f"{sec} 0x{v:08X} 0x{sz:X}"
                for (sec, v), sz in sorted(current.items())]
        OVERRIDES.write_text("\n".join(header + body) + "\n")

    print(f"gave up after {rounds} rounds")
    return 1


if __name__ == "__main__":
    sys.exit(main())
