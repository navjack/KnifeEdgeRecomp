# Reviewed game metadata

`scripts/recompile.sh` reads exactly one of these, and refuses to run without
either:

- `knife-edge.elf` — a matching MIPS ELF. Preferred when one exists, because it
  carries relocations and data symbols directly.
- `knife-edge.syms.toml` — a complete, reviewed runtime-addressed symbol map.
  This is the path that does not need an ELF.

Nothing in this directory is generated automatically. The unreviewed probe
produced by `scripts/generate_candidate_symbols.sh` lives in `decomp/` and is
gitignored; promoting it here without review is what "the recompiler produced
garbage" looks like three days later.

## Symbol map shape

```toml
[[section]]
name = "main"
rom = 0x1050
vram = 0x800C2450
size = 0x261B0
functions = [
    { name = "func_800C2450", vram = 0x800C2450, size = 0x4C },
]
```

The boot address is `0x800C2400` and must agree with whatever is supplied here
before any generated output is trusted. Section boundaries are derived in
[`../docs/ROM_ANALYSIS.md`](../docs/ROM_ANALYSIS.md).

## Recovering a map without an ELF

The route Cruis'n USA took, in order:

1. Split the ROM with Splat to get function boundaries
   (`scripts/split_rom.sh`).
2. Generate the candidate map and run the recompiler probe against it to find
   where the boundaries are wrong.
3. Capture a live memory image from an interpreter/emulator run and match it
   back to ROM ranges, which is what turns a candidate into a reviewed map —
   particularly for code that is only loaded at runtime.
4. Name libultra routines structurally, and keep a name only where the runtime
   actually provides its `<name>_recomp` replacement. Stub exception, TLB and
   handwritten-RSP routines rather than naming them.

Expect gaps: code reached only through function-pointer tables never shows up
in a live capture. Those surface later as `Failed to find function at 0x...`
on stderr, not as a compile error.
