#!/usr/bin/env python3
"""Classify proposed libultra names against what N64Recomp and the runtime do.

`map-recovery match` proposes names by structural similarity. Adopting one
blindly is dangerous, because N64Recomp treats a name as a directive:

  reimplemented_funcs  the guest body is dropped and `<name>_recomp` is called.
                       SAFE only if the runtime actually defines that symbol --
                       `osPiReadIo` is in this set with no implementation, so
                       adopting it is a guaranteed link error.
  ignored_funcs        the body is dropped and nothing is declared. Adopting one
                       SILENTLY DELETES working code.
  renamed_funcs        still recompiled, just renamed. Harmless.
  (unlisted)           ordinary recompilation. Harmless.

Reads the pinned sources so it cannot drift from the recompiler being used.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SYMBOL_LISTS = ROOT / "upstream/N64Recomp/src/symbol_lists.cpp"
RUNTIME_SRC = ROOT / "upstream/N64ModernRuntime/librecomp/src"


def parse_sets(text):
    """Extract the three name sets from symbol_lists.cpp."""
    sets = {}
    for name in ("reimplemented_funcs", "ignored_funcs", "renamed_funcs"):
        m = re.search(rf"{name}\s*(?:=|\{{)", text)
        if not m:
            continue
        # Take everything to the closing brace of this initializer.
        start = text.index("{", m.start())
        depth, i = 0, start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        sets[name] = set(re.findall(r'"([^"]+)"', text[start:i]))
    return sets


def runtime_symbols():
    """Every *_recomp symbol the runtime actually defines."""
    out = subprocess.run(
        ["grep", "-rhoE", r"\b[A-Za-z_][A-Za-z0-9_]*_recomp\s*\(", str(RUNTIME_SRC)],
        capture_output=True, text=True).stdout
    return {s.rstrip("(").strip() for s in out.splitlines()}


def load_corrections():
    """Hand-reviewed overrides of the structural match.

    `match` hashes instructions with their operands masked, so it cannot
    separate two libultra routines that differ only in an immediate -- an
    MMIO accessor's register address, for instance. Those calls need a
    person, and this is where their answer lives.
    """
    path = ROOT / "config/name-corrections.txt"
    denied, bound = set(), []
    if not path.exists():
        return denied, bound
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        verb, addr, name = line.split()
        addr = addr.lower().removeprefix("0x")
        if verb == "deny":
            denied.add((addr, name))
        elif verb == "bind":
            bound.append(("match", addr, f"func_{addr.upper()}", name))
        else:
            raise SystemExit(f"{path}: unknown directive {verb!r}")
    return denied, bound


def main():
    apply_to = None
    if "--apply" in sys.argv:
        apply_to = pathlib.Path(sys.argv[sys.argv.index("--apply") + 1])
    sets = parse_sets(SYMBOL_LISTS.read_text())
    have = runtime_symbols()
    proposals = []
    for line in sys.stdin:
        m = re.match(r"^(match|ambiguous)\s+0x([0-9A-Fa-f]+)\s+(\S+)\s+->\s+(\S+)", line)
        if m:
            proposals.append((m.group(1), m.group(2), m.group(3), m.group(4)))

    denied, bound = load_corrections()
    dropped = [p for p in proposals if (p[1].lower(), p[3]) in denied]
    proposals = [p for p in proposals if (p[1].lower(), p[3]) not in denied]
    proposals += bound

    verdicts = {}
    for status, addr, old, new in proposals:
        if new in sets.get("reimplemented_funcs", ()):
            ok = f"{new}_recomp" in have
            verdict = "ADOPT" if ok else "REJECT"
            why = "runtime implements it" if ok else "reimplemented but NO runtime impl -> link error"
        elif new in sets.get("ignored_funcs", ()):
            verdict, why = "REJECT", "in ignored_funcs -> body silently deleted"
        elif new in sets.get("renamed_funcs", ()):
            verdict, why = "SKIP", "in renamed_funcs -> no replacement, no benefit"
        else:
            verdict, why = "SKIP", "not recognized by N64Recomp -> no effect"
        if status == "ambiguous":
            verdict, why = "REJECT", "ambiguous structural match"
        verdicts.setdefault(verdict, []).append((addr, old, new, why))

    if dropped or bound:
        print(f"\n=== corrections ({len(dropped)} denied, {len(bound)} bound by hand) ===")
        for _status, addr, _old, name in sorted(dropped):
            print(f"  deny 0x{addr.upper()} {name}")
        for _status, addr, _old, name in sorted(bound):
            print(f"  bind 0x{addr.upper()} {name}")

    for verdict in ("ADOPT", "REJECT", "SKIP"):
        rows = verdicts.get(verdict, [])
        print(f"\n=== {verdict} ({len(rows)}) ===")
        for addr, old, new, why in sorted(set(rows)):
            print(f"  0x{addr} {old:<18} -> {new:<26} {why}")

    adopt_rows = verdicts.get("ADOPT", [])
    adopt = sorted({r[2] for r in adopt_rows})
    print(f"\nsafe to adopt: {len(adopt)}")
    print(" ".join(adopt))

    if apply_to is not None:
        # Write the adoptions to a file the map generator reads, NOT to the map
        # itself. The map is regenerated from Splat on every run -- converge.py
        # alone regenerates it a hundred times -- so an edit applied to the
        # generated file is silently undone, and the build quietly goes back to
        # recompiling the game's own libultra. That is exactly what happened:
        # the guest's own SI code ran instead of the runtime's and faulted
        # reading SI_STATUS at 0xA4800018, which is MMIO the runtime does not
        # map. (The map called that function osPiGetStatus; it is really
        # __osSiDeviceBusy -- see config/name-corrections.txt.)
        # Merge, never replace. `match` only reports *renames*, so once a name
        # is in the map it stops being proposed -- writing the file from one
        # run's proposals alone would silently drop every name adopted before.
        out = ROOT / "decomp/libultra-names.txt"
        merged = {}
        if out.exists():
            for line in out.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    vram, name = line.split()
                    merged[vram.lower().removeprefix("0x")] = name
        for addr, _old, new_name, _why in adopt_rows:
            merged[addr.lower()] = new_name
        for addr, name in denied:
            if merged.get(addr) == name:
                del merged[addr]
        lines = [
            "# Addresses to name as libultra functions, applied by",
            "# tools/candidate-map/make_candidate_map.py so they survive",
            "# regeneration. Produced by tools/name-filter/classify_names.py,",
            "# which keeps only names N64Recomp recognises AND the runtime",
            "# actually implements. Hand corrections live in",
            "# config/name-corrections.txt; edit those, not this file.",
        ]
        for addr in sorted(merged, key=lambda a: int(a, 16)):
            lines.append(f"0x{addr.upper()} {merged[addr]}")
        out.write_text("\n".join(lines) + "\n")
        print(f"\nwrote {len(merged)} name(s) to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
