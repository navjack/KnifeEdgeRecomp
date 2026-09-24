# Native macOS recompilation status

Updated 2026-09-24.

## Completed

- Workspace scaffolded: ROM manifest, Splat segmentation, three N64Recomp
  configurations, metadata drop location, native CMake project, shared
  SDL/Metal/RT64 frontend, reproducible patch set and dependency lock.
- The supported dump is fingerprinted four ways (`roms.toml`). Every script
  gates on the SHA-256 and refuses to run against any other ROM.
- `rom_hash` for `recomp::GameEntry` is computed and checked in:
  `56605501a85c79af`. The method was verified by reproducing Cruis'n USA's
  checked-in value from its own ROM.
- The boot stub at ROM `0x1000` is decoded, so the memory map is exact rather
  than inferred: entry `0x800C2400`, text to `0x800E85F8`, data to `0x800EEF50`,
  BSS `0x800EEF50`+`0x7BCA0`, initial `sp` `0x80126A60`, and boot continues at
  `0x800CD6B0`. `splat.yaml` encodes these boundaries.
- A ROM→RDRAM resource table was found at VRAM `0x800E8600`; its first entry
  loads ROM `0x2DB50` (exactly the end of the boot segment) to `0x8016ABF0`
  (exactly the end of BSS).
- Graphics microcode identified as stock **F3DEX2 fifo 2.05** and
  **F3DLX2 fifo 2.05.Rej**. Both are already in RT64's hash table, so no GBI
  patch is expected — unlike Cruis'n Exotica, which needed one.
- Upstream pins and four generic recompiler patches are locked with SHA-256
  fingerprints in `UPSTREAM_PINS.txt`.

## Current gate

**No reviewed game metadata exists, so no native code is generated.**

N64Recomp needs function boundaries, relocations, data symbols and overlay
information. A raw ROM provides none of those. `scripts/recompile.sh` stops with
an actionable message rather than manufacturing a placeholder binary.

## Next work, in order

1. **Split and probe.** Run `bootstrap_decomp.sh`, `split_rom.sh`, then
   `generate_candidate_symbols.sh`, and run the recompiler against
   `config/recomp-candidate.toml`. The probe's failures are the fastest way to
   find where the segment boundaries in `splat.yaml` are wrong — in particular
   the text/data split at ROM `0x27200` and the suspected handwritten-RSP window
   at `0x27178`–`0x271F8`.
2. **Decode the `0x800E8600` resource table.** How many entries, and does any
   entry carry code rather than data? Anything that does is a relocatable
   section and belongs in `config/relocatable-sections.txt`.
3. **Recover a reviewed symbol map** into `input/knife-edge.syms.toml`. See
   `input/README.md` for the route Cruis'n USA took, including the live-capture
   step that is what actually turns a candidate into a reviewed map.
4. **Locate the audio microcode** by matching the canonical `aspMain` prologue,
   and wire `M_AUDTASK` through the adapter in `rsp/`.
5. **Determine the save type** from a real access path — EEPROM through SI, or
   SRAM/FlashRAM at PI `0x08000000`. Until then `SAVE_TYPE` stays `None` in
   `CMakeLists.txt`. Do not guess it.
6. Build, package, and only then hand a bundle to a person for playtesting.

## Standing rules for this workspace

These are carried over from the other recomp ports because each one cost real
time to learn.

- **Never share another project's N64Recomp binary.** Build the one in this
  repo's `upstream/`. A mismatch runs fine, emits plausible output, and fails
  much later when generated code is compiled against the wrong runtime API.
- **Test the packaged app from outside the build tree.** librecomp creates its
  config directory with the throwing overload; a project with no writable
  app-folder path aborts before the window opens, and a build-tree run hides it
  completely.
- **Nothing but Mach-O in `Contents/MacOS`.** codesign treats any other file
  there as an unsigned nested code component and refuses to seal the bundle.
- **Strip absolute Homebrew `LC_RPATH`s from the bundled binary.** dyld searches
  rpaths in order, so a leftover `/opt/homebrew/lib` wins over the vendored
  copies while `otool -L` still looks perfect. Check with
  `otool -l <bin> | grep -A2 LC_RPATH`.
- **A quiet log is not evidence of a hang.** Trace hooks are rate-capped.
  Confirm state from window screenshots or trace-CSV counters.
- **Rule out user interaction before calling an anomaly a bug.** This machine is
  shared; a closed window and stray input are not runtime faults.
- **Claim only what was demonstrated.** A process smoke test is not gameplay,
  and a clean short boot is not a clean soak — some Metal-path bugs in this
  stack take over an hour of uptime to surface.
