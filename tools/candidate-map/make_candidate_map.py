#!/usr/bin/env python3
"""Turn Splat's disassembly into an N64Recomp candidate symbol map.

This is an EXPERIMENTAL probe, not reviewed game metadata. Function boundaries
are Splat's guesses and data symbols are unresolved, so the output is only good
for finding where the segmentation is wrong.

Splat writes three shapes in a .s file:

    nonmatching <name>, <size>   glabel <name>   instructions  -> a function
    nonmatching <name>           dlabel <name>   .word ...     -> a data blob
    (anything else)                                            -> ignored

Only the first is a function; taking every "nonmatching" line pulls data blobs
into the map with no size at all.

Two passes then fix the boundaries Splat gets wrong, in this order:

1. split  -- Splat starts a function only where it already sees one, so a
   routine reached solely through a call stays buried inside its predecessor.
   N64Recomp reports that as "No function found for jal target: 0x...".
2. merge  -- Splat ends a function at the next label, which cuts handwritten
   routines that branch across that point. N64Recomp reports that as
   "Unhandled branch ... to 0x...".

Split must run first: it is driven by call targets, which are addresses, while
merge is driven by branch edges, which survive splitting. Running merge first
lets split cut a just-merged routine straight back apart.
"""
import argparse
import pathlib
import re
import sys

LABEL_RE = re.compile(r"\.L([0-9A-Fa-f]{8})")
JAL_RE = re.compile(r"\bjal\s+func_([0-9A-Fa-f]{8})")
SEGMENT_RE = re.compile(
    r"^  - name: (?P<name>\S+)\n"
    r"    type: code\n"
    r"    start: (?P<start>0x[0-9A-Fa-f]+)\n"
    r"    vram: (?P<vram>0x[0-9A-Fa-f]+)",
    re.MULTILINE,
)


def code_segments(splat_yaml):
    """Every `type: code` segment as (name, rom_start, vram).

    `entry` and `main` match this shape too; the caller supplies those, so skip
    them here rather than emitting each section twice.
    """
    return [(m["name"], int(m["start"], 16), int(m["vram"], 16))
            for m in SEGMENT_RE.finditer(splat_yaml.read_text())
            if m["name"] not in ("entry", "main")]


def parse_file(path):
    """(functions, branch_edges) for one Splat .s file.

    functions is [(name, vram, size)]; branch_edges is [(from_addr, to_addr)]
    for every intra-file branch, which is what the merge pass needs after the
    split pass has moved the function boundaries around.
    """
    funcs, edges, blobs = [], [], []
    name = size = None
    want_label = pending_kind = None
    addr = None
    for line in path.read_text().splitlines():
        if line.startswith("nonmatching "):
            fields = line.split()
            pending_kind = None
            # A function carries a size; a data blob does not.
            if len(fields) == 3:
                name, size = fields[1].rstrip(","), fields[2]
                want_label = "func"
            else:
                want_label = "data"
            continue
        if want_label:
            if not line.strip():
                continue
            kind, want_label = want_label, None
            if kind == "func" and line.startswith("glabel "):
                pending_kind = "func"
            elif kind == "data" and line.startswith("dlabel "):
                pending_kind = "data"
            continue
        stripped = line.lstrip()
        if not stripped.startswith("/*"):
            continue
        parts = stripped.split()
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[2], 16)
        except ValueError:
            continue
        if pending_kind == "func":
            # The game's own main must not collide with the host program's.
            funcs.append([("func_" + parts[2]) if name == "main" else name,
                          addr, int(size, 16)])
            pending_kind = None
        elif pending_kind == "data":
            blobs.append(addr)
            pending_kind = None
        for m in LABEL_RE.finditer(line):
            edges.append((addr, int(m.group(1), 16)))
    # Splat's declared size routinely overruns whatever comes next:
    # func_8016D6F0 is listed as 0x5E4 while func_8016D8A4 starts 0x1B4 in, and
    # func_8017B96C is listed as 0x12C while the real body is four instructions
    # followed by the data blob D_8017B980. Overlapping ranges make every later
    # pass wrong, and recompiling into data raises bogus "Unhandled instruction"
    # errors, so clamp each function to the next boundary of either kind.
    funcs.sort(key=lambda f: f[1])
    bounds = sorted(set(blobs) | {f[1] for f in funcs})
    import bisect
    for f in funcs:
        i = bisect.bisect_right(bounds, f[1])
        if i < len(bounds):
            f[2] = min(f[2], bounds[i] - f[1])
    return [tuple(f) for f in funcs if f[2] > 0], edges


def call_targets(path):
    """Every address a `jal` in this one file points at."""
    return {int(m.group(1), 16) for m in JAL_RE.finditer(path.read_text())}


def split_at_targets(funcs, targets):
    """Start a function at every call target that lands mid-function."""
    out = []
    for name, vram, size in funcs:
        cuts = sorted(t for t in targets if vram < t < vram + size)
        start = vram
        for cut in cuts:
            out.append((name if start == vram else f"func_{start:08X}", start, cut - start))
            start = cut
        out.append((name if start == vram else f"func_{start:08X}", start, vram + size - start))
    return out


def merge_across_branches(funcs, edges, protected):
    """Extend each function until every branch out of it lands inside it.

    Splat ends a function at the next label, which cuts handwritten routines
    that branch across that point; N64Recomp then rejects the whole function
    with "Unhandled branch ... to 0x...". `.L` labels are intra-function by
    construction, so a target past the end always means the boundary is wrong.

    Functions that something calls are never absorbed -- a call target is an
    entry point, and removing it puts N64Recomp back to "No function found for
    jal target". Where the two rules disagree, the call target wins: a branch
    only has to land inside *a* function, while a call has to resolve to an
    exact address.
    """
    if not funcs:
        return funcs
    funcs = sorted(funcs, key=lambda f: f[1])
    by_src = {}
    for src, dst in edges:
        by_src.setdefault(src, []).append(dst)

    section_end = max(v + sz for _, v, sz in funcs)
    out = []
    i = 0
    while i < len(funcs):
        name, vram, size = funcs[i]
        while True:
            end = vram + size
            reach = max((d for src, dsts in by_src.items() if vram <= src < end
                         for d in dsts), default=0)
            if reach < end:
                break
            # Extend to cover the target. Splat sometimes calls the bytes in
            # between data, so there may be no adjacent function to absorb --
            # extend anyway, but never past the next call target, which is a
            # real entry point, nor past the end of the section.
            limit = min((f[1] for f in funcs[i + 1:]
                         if f[1] in protected and f[1] > vram), default=section_end)
            new_end = min(reach + 4, limit)
            if new_end <= end:
                break
            size = new_end - vram
            while i + 1 < len(funcs) and funcs[i + 1][1] < vram + size:
                i += 1
        out.append((name, vram, size))
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    args = ap.parse_args()

    root = args.root
    asm = root / "asm"
    # Overlays share destination addresses, so call targets are scoped rather
    # than pooled: an address that is an entry point in one overlay lands
    # mid-function in another, and a global set splits both. Each section sees
    # its own calls plus the resident segment's, which is what can call into it.
    resident_targets = call_targets(asm / "1050.s")

    segs = [("entry", 0x1000, 0x800C2400), ("main", 0x1050, 0x800C2450)]
    segs += code_segments(root / "splat.yaml")

    chunks = [
        "# Candidate map generated from Splat control-flow boundaries by",
        "# tools/candidate-map/make_candidate_map.py.",
        "#",
        "# This is NOT a reviewed symbol map. Function boundaries are Splat's",
        "# guesses, refined by a split pass (call targets) and a merge pass",
        "# (branches crossing boundaries); data symbols are unresolved. The ovl_*",
        "# sections share destination addresses and must be marked relocatable.",
    ]
    total = split_total = 0
    for name, rom, vram in segs:
        src = asm / f"{rom:X}.s"
        if not src.exists():
            print(f"missing Splat output: {src}", file=sys.stderr)
            return 2
        funcs, edges = parse_file(src)
        raw = len(funcs)
        # A call from inside this overlay is unambiguously an entry point here,
        # so it is protected from merging. A call from the resident segment may
        # well mean the *other* overlay mapped at the same vram, so it is only a
        # hint: split on it, but let a crossing branch merge it back. N64Recomp
        # resolves such ambiguous targets at runtime anyway.
        # Every call target is protected from being absorbed. Where a crossing
        # branch and a call target genuinely conflict -- routines sharing a tail
        # -- the call wins and the branch is left unresolved, because a call has
        # to resolve to an exact address while a branch only needs to land
        # inside some function. Those few functions are reported by N64Recomp as
        # "Unhandled branch" and are a known limit of an unreviewed map.
        targets = call_targets(src) | resident_targets
        funcs = merge_across_branches(
            split_at_targets(funcs, targets), edges, targets)
        total += len(funcs)
        split_total += len(funcs) - raw
        chunks += ["", "[[section]]", f'name = "{name}"',
                   f"rom = 0x{rom:X}", f"vram = 0x{vram:X}"]
        end = max((v + s for _, v, s in funcs), default=vram)
        chunks.append(f"size = 0x{end - vram:X}")
        chunks.append("functions = [")
        for fname, fvram, fsize in sorted(funcs, key=lambda f: f[1]):
            chunks.append(f'    {{ name = "{fname}", vram = 0x{fvram:X}, size = 0x{fsize:X} }},')
        chunks.append("]")

    args.out.write_text("\n".join(chunks) + "\n")
    print(f"{len(segs)} sections, {total} functions "
          f"({split_total:+d} from boundary fixes) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
