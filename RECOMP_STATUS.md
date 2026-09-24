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

## Overlay recovery (2026-09-24)

The overlay layout is fully recovered, which was the blocker for everything
else. `tools/overlay-map/overlay_map.py` reads it straight out of the ROM:
**36 descriptors, 17 of them code**, across 5 destinations that are reused by
more than one overlay. The resident segment at `0x800C2400` is only the engine
half; the gameplay code is a 205 KB module DMA'd to `0x8016D6F0`.

`splat.yaml` now carries every overlay as its own segment. Recognised code went
from 1.86% of the cartridge to 21.4%, and the candidate map from 566 functions
in one section to **1,264 across 19 sections**. All of it is documented in
`docs/ROM_ANALYSIS.md`, including the 0x28-byte descriptor struct and the four
loaders that consume it.

`tools/candidate-map/make_candidate_map.py` replaces the old awk generator and
corrects four distinct ways Splat's boundaries are wrong: overlapping declared
sizes, sizes that run past a function into a data blob, missing entry points for
routines only ever reached by a call, and functions cut in half by a branch that
crosses the cut.

ROM `0x23DF0`–`0x27200` turned out to be RSP microcode embedded in the resident
segment (895 COP2 words, zero MIPS prologues) and is now marked `bin`.

## Phase 1-2: recovery tooling and the libultra boundary (2026-09-24)

`tools/map-recovery` and `tools/recomp-boundary-audit` are ported from the
Cruis'n workspace and build with Go 1.26. `scripts/recover_map.sh` runs
`match` against all three reviewed Cruis'n maps, then `sweep` and `locate`.

**`locate` reports 0 external jal targets**, so no overlay is hiding in unmapped
ROM: the 17 account for every call in the image.

The candidate generator now also splits on cross-overlay call targets — the
gameplay module calls the level modules, and those targets appear in no file
that maps the callee. That took the map from 1,264 to 2,314 functions and
`sweep`'s unmapped count from 9 to **3** (one function-pointer constant and two
calls into the level overlays past the disassembled extent).

**34 libultra functions are named and bound**, filtered by
`tools/name-filter/classify_names.py`, which reads N64Recomp's own
`symbol_lists.cpp` and the runtime's actual `*_recomp` symbols. It rejected 13
of the 47 proposals, and every rejection mattered:

- `osPiReadIo` — in `reimplemented_funcs` with **no runtime implementation**;
  adopting it is a guaranteed link error.
- seven names in `ignored_funcs` (`__osProbeTLB`, `__osDispatchThread`,
  `__osEnqueueAndYield`, `osPiRawStartDma`, `osPiWriteIo`, `osYieldThread`,
  `__osViInit`) — adopting one **silently deletes** working code.
- two ambiguous structural matches, and one misspelled variant
  (`osWriteBackDCacheAll`) that N64Recomp would not recognize at all.

Generated code now calls the runtime for real: `osRecvMesg_recomp` (47 sites),
`osSendMesg_recomp` (33), `osCreateThread_recomp` (11), `osSpTaskLoad_recomp`,
`osSpTaskStartGo_recomp`, `osViSwapBuffer_recomp`. The match also independently
confirmed the hand identification of `func_800D1D10`'s callees as
osCreateMesgQueue / osInvalDCache / osRecvMesg.

## Current gate

**The candidate probe recompiles 2,313 of 2,314 functions.** The one holdout is
a function in `ovl_02FC30` whose `.L` branch crosses a protected call target —
two routines sharing a tail. Splitting and merging genuinely disagree there, and
the call target wins because a call must resolve to an exact address while a
branch only has to land inside some function.

That number is a diagnostic, not a milestone: the candidate map is explicitly
unreviewed, with no data symbols and no relocations, so it cannot produce a
working game. It exists to show where the segmentation is still wrong, and it
has now done most of that job.

## Next work, in order

1. **Recover a reviewed symbol map** into `input/knife-edge.syms.toml`. This is
   the long pole. See `input/README.md` for the route Cruis'n USA took,
   including the live-capture step that is what actually turns a candidate into
   a reviewed map. The section layout is no longer the unknown; function
   boundaries, data symbols and relocations are.
2. **Write the `rsp/` adapter.** RT64 handles F3DEX2 itself, so the first
   version only has to route graphics tasks and refuse the rest loudly.
3. **Locate the audio microcode** inside the `0x23DF0`–`0x27200` block by
   matching the canonical `aspMain` prologue, then wire `M_AUDTASK` through the
   adapter.
4. **Bind the libultra boundaries** — `osContStartReadData`, `osContGetReadData`,
   EEPROM/Pak I/O — to their runtime replacements. Cruis'n USA needed about a
   dozen, and getting them wrong is what kept it off its title screen.
5. **Settle the save type** from a real access path. SRAM and FlashRAM are ruled
   out; confirm EEPROM or None. Until then `SAVE_TYPE` stays `None`.
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
