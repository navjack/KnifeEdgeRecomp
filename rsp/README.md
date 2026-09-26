# Title RSP microcode

`scripts/build_app.sh` refuses to build with this directory empty, and
`native/rsp_contract.hpp` requires an adapter:

```c
extern "C" RspUcodeFunc* knife_edge_get_rsp_microcode(const OSTask* task);
```

The adapter routes each `OSTask` to a real recompiled microcode implementation.
An app that silently runs with no RSP implementation is worse than one that
fails to configure, which is why the gate is here.

## Graphics

Knife Edge uses stock **F3DEX2 fifo 2.05** and **F3DLX2 fifo 2.05.Rej**
(see [`../docs/ROM_ANALYSIS.md`](../docs/ROM_ANALYSIS.md) for the ROM offsets of
their identifying strings). RT64 handles graphics tasks itself and identifies
the revision by hashing the loaded ucode, so graphics tasks do not need a
recompiled microcode here — but confirm that from RT64's own identification at
runtime rather than assuming it.

## Audio

**Stock `aspMain`, recompiled from ROM `0x23EC0`.** Config in
[`aspMain.us.toml`](aspMain.us.toml). The generated `aspMain.cpp` is a
translation of the game's own microcode, so it is gitignored like `generated/`;
`scripts/build_app.sh` regenerates it from the ROM through
`scripts/generate_rsp.sh`.

It was located from the running game rather than by searching. The adapter logs
the ucode pointer out of the first `M_AUDTASK` it sees; that was guest
`0x800E52C0`, and since the resident segment maps ROM `0x1000` to VRAM
`0x800C2400`, the ROM offset is `0x23EC0` — inside the `0x23DF0`–`0x27200` block
[`../docs/ROM_ANALYSIS.md`](../docs/ROM_ANALYSIS.md) had already picked out from
its COP2 density. That is worth preferring to a prologue search when the game
can be made to run at all: it gives the exact address with no pattern to match.

Confirmation came from comparing against a known-stock copy: the `0xE20` bytes
there are byte-identical to the `aspMain` in Cruis'n USA and Cruis'n World, and
the identical run is *exactly* `0xE20` long, which is also why the text size is
not a guess. The generated `aspMain.cpp` is in turn byte-identical to Cruis'n
USA's, so the same revision needs the same 16 indirect branch targets.

Those targets matter: the command dispatcher jumps through a 16-entry DMEM
table, which the recompiler cannot follow statically, so without them roughly
half the audio commands exit with `UnhandledJumpTarget` at run time. A 1,800-VI
soak reports none.

To redo this for another title:

1. Let the adapter log the ucode pointer from the first `M_AUDTASK`, and
   convert it to a ROM offset through the resident segment's mapping. Failing
   that, search for the canonical stock `aspMain` prologue bytes.
2. Compare the full text block against a known-stock copy byte for byte, and
   take the text size from where that comparison stops matching.
3. Record offset, size and the IMEM load address (`0x04001080`) in a toml, and
   generate with RSPRecomp. Note `output_file_path` needs a directory component:
   RSPRecomp calls `create_directories` on the parent path, and a bare filename
   has an empty parent, which throws.
4. Route `M_AUDTASK` to it from the adapter.
