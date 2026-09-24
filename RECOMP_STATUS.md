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

## Current gate (2026-09-24, evening)

**The game boots, loads overlays at runtime, and holds a per-frame loop.** A
600-frame bounded run exits cleanly, having loaded two overlays, executed
overlay code, submitted display lists through `osSpTaskStartGo`, and queued an
audio buffer on every frame. What has *not* been established is whether what
reaches RT64 is a correct frame; nobody has looked at the window.

Getting here was four boundary problems, and the shape of each is worth
keeping, because the same shapes will recur:

- **The libultra boundary is not just the functions that touch hardware.**
  Listing every function in the resident segment that reads an MMIO register
  and walking each one up its call graph bound seven entry points and took
  eight hardware leaves off the live path at once. But it missed
  `osEPiStartDma`, which touches no register: it posts to the PI device
  manager and blocks. Binding `osCreatePiManager` had already replaced that
  manager with the runtime's empty one, so every DMA waited forever. Binding a
  function that creates a service thread obliges you to bind every client that
  posts to it.

- **A name matched structurally is a hypothesis, not a fact.** `map-recovery
  match` hashes instructions with the operands masked, so it cannot separate
  two routines that differ only in an immediate: it called `__osSiDeviceBusy`
  `osPiGetStatus`, because SI_STATUS and PI_STATUS differ only in a `lui`.
  Every MMIO accessor needs a person to read the register address before its
  name is trusted. Hand corrections live in `config/name-corrections.txt`.

- **Guest busy-waits deadlock this runtime.** ultramodern drains its external
  message queue only at the *start* of an os* call, and schedules guest threads
  one at a time until one calls into the OS. A loop that calls nothing does
  both fatal things at once. `tools/spin-scan/find_spin_loops.py` finds them by
  shape and emits the hook entries;
  [`native/game/guest_waits.cpp`](native/game/guest_waits.cpp) supplies the
  poll. Note that delivering the message is only half of it -- the woken thread
  does not run until the spinning one yields, which is why every os* entry
  point ends in `check_running_queue`.

- **Bounded runs must not unwind.** `recomp::start` joins the game and event
  threads but never the guest OSThreads, so it unmapped RDRAM underneath them
  and every `--stop-after-vi` run ended in a SIGSEGV unrelated to the game.
  Enough of those got macOS prompting to reopen windows on launch, which blocks
  in `SDL_CreateWindow` before anything renders and cost a good while to
  recognise. The flag now `_Exit`s after flushing, and the bundle opts out of
  persistent UI.

**Audio works.** The stock `aspMain` at ROM `0x23EC0` is recompiled by
RSPRecomp and routed from the adapter; a 1,800-VI (30 s) run produces 1.91M
non-zero samples, 99.3% of a continuous 32 kHz stereo stream, with no unhandled
jump targets. It was found from the running game -- the adapter logged the ucode
pointer out of the first `M_AUDTASK` -- rather than by pattern-searching the
ROM. See [`rsp/README.md`](rsp/README.md).

The candidate map remains explicitly unreviewed -- no data symbols, no
relocations. Everything above runs on it, which says the segmentation is
broadly right; it does not make the map correct.

## Next work, in order

1. **Look at a frame.** Nothing so far distinguishes "the game submits display
   lists" from "the display lists are right". Watch RT64's stderr for a GBI
   mismatch on the first real task.
2. **Reach the gameplay overlay.** `func_800CC690` loads the 205 KB module at
   `0x8016D6F0`; expect the second overlay tier at `0x801D21F0` right after,
   six of which no resident call site references.
3. **Bind the remaining input boundaries** -- `osContGetReadData`, Pak I/O --
   and author `config/runtime-boundaries.json` so the audit is a build gate.
4. **Recover a reviewed symbol map** into `input/knife-edge.syms.toml`. Still
   the long pole, and still what separates a probe from a port.
5. **Settle the save type** from a real access path. SRAM and FlashRAM are
   ruled out; confirm EEPROM or None. Until then `SAVE_TYPE` stays `None`.

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
