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


def merge_across_branches(funcs, edges, protected, section_name,
                          rom_bytes, sec_rom, sec_vram):
    """Widen each function until every branch out of it lands inside it.

    Splat ends a function at the next label, which cuts routines that branch
    across that point; N64Recomp then rejects the whole function with
    "Unhandled branch ... to 0x...". `.L` labels are intra-function by
    construction, so a target past the end always means the boundary is wrong.

    Widening, rather than absorbing, is what handles a multiple-entry routine.
    `func_8016DCD4` is an `alabel` -- an alternate entry, called from three
    sites -- sitting inside a routine that also branches across it. Absorbing it
    loses the entry point ("No function found for jal target"); refusing to
    cross it loses the branch. Both are satisfied by letting the ranges overlap:
    each entry is recompiled from its own start, and the shared tail is emitted
    twice, which is exactly what the original assembly means.

    Functions that nothing calls and that end up wholly inside a widened
    neighbour are Splat artifacts and are dropped, so overlap stays confined to
    real alternate entries.
    """
    if not funcs:
        return funcs
    funcs = sorted(funcs, key=lambda f: f[1])
    section_end = max(v + sz for _, v, sz in funcs)
    # A branch needs one function containing both of its ends, so widen in both
    # directions: forward for a branch leaving the bottom, and -- for a branch
    # arriving from below the start -- widen whichever function owns the target
    # until it also covers the source.
    by_src = {}
    by_dst = {}
    for src, dst in edges:
        by_src.setdefault(src, []).append(dst)
        by_dst.setdefault(dst, []).append(src)

    # Widening must stay bounded. A `.L` reference can chain far enough to run a
    # function off the end of the code and into data, where N64Recomp decodes a
    # data word as a branch to somewhere impossible ("Unhandled branch ... to
    # 0x883EAA58"). Splat's own extent is the anchor; allow generous growth for
    # a split routine but not unbounded growth.
    MAX_GROWTH = 0x1000
    original_end = {vram: vram + size for _, vram, size in funcs}
    widened = [[name, vram, size] for name, vram, size in funcs]
    changed = True
    while changed:
        changed = False
        for entry in widened:
            _, vram, size = entry
            end = vram + size
            forward = max((d for src, dsts in by_src.items() if vram <= src < end
                           for d in dsts), default=0)
            backward = max((src for dst, srcs in by_dst.items() if vram <= dst < end
                            for src in srcs), default=0)
            reach = max(forward, backward)
            if reach < end:
                continue
            new_end = min(reach + 4, section_end,
                          original_end[vram] + MAX_GROWTH)
            if new_end > end and not region_is_code(
                    rom_bytes, sec_rom + (end - vram), sec_rom + (new_end - vram)):
                continue
            if new_end > end:
                entry[2] = new_end - vram
                changed = True

        # A body that branches above its own start can only be absorbed, never
        # made self-contained. Widen whichever function owns the target so it
        # covers this one entirely -- covering just the branch is not enough,
        # since the enclosing function has to swallow the whole alternate entry
        # for it to be droppable below.
        for _, vram, size in widened:
            end = vram + size
            escapes = [dst for src, dsts in by_src.items() if vram <= src < end
                       for dst in dsts if dst < vram]
            if not escapes:
                continue
            target = min(escapes)
            owner = None
            for candidate in widened:
                if candidate[1] <= target and (owner is None or candidate[1] > owner[1]):
                    owner = candidate
            if owner is not None and owner[1] + owner[2] < end:
                capped = min(end, section_end,
                             original_end[owner[1]] + MAX_GROWTH)
                if capped > owner[1] + owner[2]:
                    owner[2] = capped - owner[1]
                    changed = True

    out = []
    for name, vram, size in widened:
        size = SIZE_OVERRIDES.get((section_name, vram), size)
        if size == 0:
            # An override of 0 drops the entry: Splat called a stretch of data a
            # function, and no valid extent exists for it.
            continue
        end = vram + size
        covered_by = [(v, sz) for _, v, sz in widened
                      if v < vram and v + sz >= end]

        # An alternate entry whose body branches above its own start cannot be
        # expressed: a recompiled function may not branch outside itself, and
        # its start is fixed because something calls it. func_8016FBCC is one --
        # an `alabel` at 0x8016FBCC branching back to 0x8016FBC4, eight bytes
        # earlier. The enclosing function already covers both ends correctly, so
        # drop the entry and let N64Recomp's resolve_jal create a static
        # function for the call site instead.
        escapes_below = any(vram <= src < end and dst < vram
                            for src, dsts in by_src.items() for dst in dsts)
        if escapes_below and covered_by:
            continue

        # Anything nothing calls that ends up wholly inside a widened neighbour
        # is a Splat artifact, so overlap stays confined to real alternate
        # entries.
        if vram not in protected and covered_by:
            continue
        out.append((name, vram, size))
    return out


JR_RA = 0x03E00008


# Primary opcodes IDO actually emits. Data decodes as a scatter of everything.
COMMON_OPS = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x14, 0x15, 0x16, 0x17,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E,
    0x31, 0x35, 0x39, 0x3D, 0x37, 0x3F,
}


def region_is_code(rom_bytes, off, end_off):
    """Is this stretch plausibly instructions rather than data?

    Widening a function has to stop at the end of the code. When it does not,
    a data word gets decoded as an instruction and the damage shows up far
    away: as `movn` in ovl_497140 (an instruction that appears nowhere in the
    disassembly), or as a `jal` to 0x801E709C that no Splat file contains,
    which makes N64Recomp invent a static function over yet more data.
    """
    import struct as _struct
    span = rom_bytes[off:end_off]
    words = [_struct.unpack(">I", span[i:i + 4])[0]
             for i in range(0, len(span) - 3, 4)]
    if not words:
        return True
    run = best = 0
    for w in words:
        run = run + 1 if w == 0 else 0
        best = max(best, run)
    if best >= 8:                       # padding or a zeroed table
        return False
    known = sum(1 for w in words if (w >> 26) in COMMON_OPS)
    return known >= len(words) * 6 // 10


def is_jump_or_branch(word):
    """Does this word have a delay slot?"""
    op = word >> 26
    if op in (0x02, 0x03):                      # j, jal
        return True
    if op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
        return True                             # beq/bne/blez/bgtz and likely
    if op == 0x01:                              # regimm: bltz/bgez/bltzal/...
        return True
    if op == 0x11 and ((word >> 21) & 0x1F) == 0x08:
        return True                             # bc1f/bc1t
    if op == 0x00 and (word & 0x3F) in (0x08, 0x09):
        return True                             # jr, jalr
    return False


def include_delay_slots(funcs, rom_bytes, sec_rom, sec_vram, sec_end):
    """Never let a function end between a jump and its delay slot.

    N64Recomp emits `goto after_N;` for a call in a delay slot and puts the
    `after_N:` label on the instruction that follows. If the boundary falls
    between the two, the label lands in the next function and the generated C
    fails to compile with "use of undeclared label". Splitting at a call target
    can land exactly there.
    """
    import struct as _struct
    out = []
    for name, vram, size in funcs:
        end = vram + size
        while end < sec_end:
            off = sec_rom + (end - 4 - sec_vram)
            if off < 0 or off + 4 > len(rom_bytes):
                break
            if not is_jump_or_branch(_struct.unpack(">I", rom_bytes[off:off + 4])[0]):
                break
            end += 4
        out.append((name, vram, end - vram))
    return out


def looks_like_code(rom_bytes, off, limit):
    """Do the bytes at this offset plausibly begin a function?

    Overlays share destinations, so a call from one overlay names an address
    that is data in most of the others. Creating a function there -- whether by
    splitting an existing one or by inventing a new one -- hands N64Recomp data
    to decode, which surfaces as nonsense like "Unhandled instruction: movn",
    an instruction that appears nowhere in the whole disassembly.
    """
    import struct as _struct
    window = rom_bytes[off:min(off + 0x400, limit)]
    words = [_struct.unpack(">I", window[i:i + 4])[0]
             for i in range(0, len(window) - 3, 4)]
    if not words:
        return False
    if (words[0] & 0xFFFF8000) == 0x27BD8000:      # stack-adjust prologue
        return True
    return any((w & 0xFFFF8000) == 0x27BD8000 or w == JR_RA for w in words)

# Addresses this generator cannot produce a valid function for, with the reason.
# Each one is a real routine, not a boundary mistake -- the shapes simply have no
# expression as a standalone recompiled function. Dropping the entry leaves the
# call sites to N64Recomp's resolve_jal, which creates a static function at the
# target when it is in the calling section.
#
# This is a candidate-map limitation. A reviewed map resolves these by hand.
# Function extents this generator gets wrong, as `0xVRAM 0xSIZE` lines in
# decomp/size-overrides.txt. Dropping such a function is not an option when its
# callers live in another section, because resolve_jal can only create a static
# for a call from within the same section.
#
# The file is produced by tools/candidate-map/converge.py, which reads the exact
# extent each failure requires straight out of N64Recomp's own diagnostics
# rather than guessing.
def load_size_overrides(root):
    path = root / "decomp/size-overrides.txt"
    overrides = {}
    if not path.exists():
        return overrides
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        section, vram, size = line.split()
        # Keyed by section as well as address: eight overlays share 0x8017B0C0,
        # and an extent that is right in one is data in another.
        overrides[(section, int(vram, 16))] = int(size, 16)
    return overrides


SIZE_OVERRIDES = {}


def synthesize_missing(funcs, targets, rom_bytes, sec_rom, sec_vram, sec_size):
    """Invent a function at any call target no function covers.

    Splat only disassembles what it recognises as code, so a routine reached
    solely from another overlay can sit in a stretch it filed as data. Nothing
    can then split it out, and N64Recomp fails the caller with "No function
    found for jal target". Scan from the target to its terminating `jr $ra` and
    add a function, but only when the bytes really do end like a function --
    otherwise this would paper over a genuinely wrong section boundary.
    """
    import struct
    covered = [(v, v + sz) for _, v, sz in funcs]
    starts = {v for _, v, _ in funcs}
    added = []
    for target in sorted(targets):
        if not (sec_vram <= target < sec_vram + sec_size):
            continue
        if target in starts or any(lo <= target < hi for lo, hi in covered):
            continue
        off = sec_rom + (target - sec_vram)
        limit = min(off + 0x2000, sec_rom + sec_size, len(rom_bytes))

        # Only invent a function where the bytes actually look like one.
        # Overlays share destinations, so a call from one overlay names an
        # address that is data in most of the others; synthesising there
        # produces a function over data, which N64Recomp then chokes on with
        # things like "Unhandled instruction: movn" -- an instruction that
        # appears nowhere in the whole disassembly.
        if not looks_like_code(rom_bytes, off, limit):
            continue

        end = None
        for pc in range(off, limit - 4, 4):
            if struct.unpack(">I", rom_bytes[pc:pc + 4])[0] == JR_RA:
                end = pc + 8  # include the delay slot
                break
        if end is None:
            continue
        # Do not run into a function that already exists.
        following = [v for _, v, _ in funcs if v > target]
        if following:
            end = min(end, sec_rom + (min(following) - sec_vram))
        size = end - off
        if size <= 0:
            continue
        added.append((f"func_{target:08X}", target, size))
    return funcs + added


def descriptor_sizes(root):
    """ROM start -> loaded size, for each code overlay, from the ROM's own table.

    Splat only disassembles the part of a module it recognises as code, so a
    section sized from the function extent understates the module -- by 75% for
    ovl_3EFDD0. The descriptor is authoritative about how many bytes are loaded,
    and N64Recomp uses the section size to decide what a section contains.
    """
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "overlay_map", root / "tools/overlay-map/overlay_map.py")
    om = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(om)
    rom = (root / "rom/Knife Edge - Nose Gunner (U) [!].z64").read_bytes()
    return {d[3]: d[4] - d[3] for d in om.scan(rom)
            if om.classify(rom, d[3], d[4])[0] == "code"}


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
    # Overlays call into other overlays: the gameplay module at 0x8016D6F0 calls
    # the level modules at 0x801D21F0. Those targets appear in no file that maps
    # the callee, so a per-file target set leaves them without an entry point and
    # `map-recovery sweep` reports them as unmapped. Split on every call target in
    # the image that lands inside a section; only same-file targets are protected
    # from merging, since a cross-overlay call is ambiguous between the overlays
    # sharing that destination and N64Recomp resolves it at runtime anyway.
    all_targets = set()
    for src in sorted(asm.glob("*.s")):
        all_targets |= call_targets(src)

    segs = [("entry", 0x1000, 0x800C2400), ("main", 0x1050, 0x800C2450)]
    segs += code_segments(root / "splat.yaml")
    desc_sizes = descriptor_sizes(root)
    rom_bytes = (root / "rom/Knife Edge - Nose Gunner (U) [!].z64").read_bytes()
    SIZE_OVERRIDES.update(load_size_overrides(root))

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
        # Split on every call target that lands in this section, wherever it
        # was found. Overlays call into other overlays -- the gameplay module at
        # 0x8016D6F0 calls the level modules at 0x801D21F0 -- and those targets
        # appear in no file that maps the callee, so a per-file set leaves them
        # without an entry point and `map-recovery sweep` flags them.
        #
        # Every call target is also protected from merging. N64Recomp's
        # resolve_jal needs the target to begin a function in some section that
        # covers it; if a merge absorbs one, the call fails outright with "No
        # function found for jal target". Where a crossing branch and a call
        # target genuinely conflict -- routines sharing a tail -- the call wins,
        # and the branch is left for N64Recomp to report as "Unhandled branch".
        # That is a known limit of an unreviewed map, not something to paper over.
        lo = vram
        hi = max((v + sz for _, v, sz in funcs), default=vram)
        sec_size = desc_sizes.get(rom, hi - lo)
        # A cross-overlay call only becomes an entry point here if the bytes at
        # that address in THIS overlay are code; otherwise the call means one of
        # the other overlays sharing the destination.
        cross = {t for t in all_targets if lo <= t < hi
                 and looks_like_code(rom_bytes, rom + (t - vram),
                                     rom + sec_size)}
        targets = call_targets(src) | resident_targets | cross
        funcs = split_at_targets(funcs, targets)
        funcs = synthesize_missing(funcs, targets, rom_bytes, rom, vram,
                                   desc_sizes.get(rom, hi - lo))
        funcs = merge_across_branches(funcs, edges, targets, name,
                                      rom_bytes, rom, vram)
        funcs = include_delay_slots(funcs, rom_bytes, rom, vram,
                                    vram + desc_sizes.get(rom, hi - lo))
        total += len(funcs)
        split_total += len(funcs) - raw
        chunks += ["", "[[section]]", f'name = "{name}"',
                   f"rom = 0x{rom:X}", f"vram = 0x{vram:X}"]
        # The ROM descriptor says how many bytes are loaded, and nothing may
        # claim more: letting a function extent widen the section once inflated
        # ovl_1B66F0 to 136 MB after a data word was decoded as a branch.
        extent = max((v + s for _, v, s in funcs), default=vram)
        size = desc_sizes.get(rom) or (extent - vram)
        chunks.append(f"size = 0x{size:X}")
        chunks.append("functions = [")
        section_end = vram + size
        funcs = [(n, v, min(sz, section_end - v)) for n, v, sz in funcs
                 if v < section_end and min(sz, section_end - v) > 0]
        for fname, fvram, fsize in sorted(funcs, key=lambda f: f[1]):
            # Overlays that share a destination produce the same func_XXXXXXXX
            # name in several sections, and the C generator emits one definition
            # per function -- duplicates are a link error, which N64Recomp itself
            # never reports. Splat's names are addresses, not identities, so
            # qualify every overlay function with its section. Resident names,
            # including the adopted libultra ones, are already unique.
            emitted = fname if name in ("entry", "main") else f"{name}_{fname}"
            chunks.append(f'    {{ name = "{emitted}", vram = 0x{fvram:X}, size = 0x{fsize:X} }},')
        chunks.append("]")
        # NOTE: deliberately no `relocs` array here.
        #
        # `relocatable_sections_path` is inert in symbol-map mode --
        # Context::from_symbol_file marks a section relocatable only when a
        # `relocs` array is present (upstream/N64Recomp/src/config.cpp:649) --
        # but marking these relocatable is WRONG without real reloc data.
        # resolve_jal (src/recompilation.cpp:63) only accepts a candidate whose
        # section is NOT relocatable, so every resident -> overlay call would
        # resolve to nothing and fail outright.
        #
        # Left non-relocatable, N64Recomp does the right thing on its own: a
        # target that several overlays claim resolves Ambiguous and is emitted
        # as a runtime LOOKUP_FUNC, and a target only one overlay claims is a
        # direct call, which is correct because only that overlay can be loaded
        # there. The residual risk is a target whose true owner is missing a
        # boundary in the map -- which is what `map-recovery sweep` exists to
        # find, and why its count is a build-time gate.

    args.out.write_text("\n".join(chunks) + "\n")
    print(f"{len(segs)} sections, {total} functions "
          f"({split_total:+d} from boundary fixes) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
