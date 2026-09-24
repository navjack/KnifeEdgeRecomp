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

Not yet located. There is no `aspMain` name string in the ROM, which is normal.
The technique that worked for Cruis'n USA:

1. Search the ROM for the canonical stock `aspMain` prologue bytes.
2. Compare the full text block against a known-stock copy, byte for byte.
3. Record the ROM offset, text size and the IMEM load address in an
   `aspMain.us.toml`, and generate the implementation with RSPRecomp.
4. Route `M_AUDTASK` to it from the adapter.

The audio command dispatcher jumps through a 16-entry DMEM table, so the
toml also needs `extra_indirect_branch_targets` listing each handler entry
point, or the recompiled microcode will be missing half its commands.
