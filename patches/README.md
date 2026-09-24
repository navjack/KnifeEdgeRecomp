# Upstream patches

Patches in this directory are applied by `scripts/build_stack.sh` against the
checkouts pinned in `UPSTREAM_PINS.txt`, and their SHA-256 fingerprints are
recorded there too. Every one is guarded: the script checks for a marker string
in the target file first, so re-running it is a no-op, and it refuses to patch
over unrelated local edits.

All four carried here are generic MIPS/recompiler correctness fixes with no
game-specific naming. They came from the Cruis'n workspace, where they are in
daily use.

| Patch | What it does |
| --- | --- |
| `N64Recomp-cache-noop.patch` | Emits `cache` as a documented no-op instead of failing; N64 cache maintenance has no host-side meaning after static recompilation. |
| `N64Recomp-cop0-registers.patch` | Adds a 32-entry COP0 file to `recomp_context` and routes `mfc0`/`mtc0` for registers other than Status through it, instead of aborting recompilation. |
| `N64Recomp-fpu64-conversions.patch` | Adds the missing `trunc.l`/`round.l`/`ceil.l`/`floor.l` conversions for both `.s` and `.d`. |
| `N64Recomp-zero-register-writes.patch` | Drops writes to `$zero`, which IDO emits as memory probes (`lw $zero, 0(reg)`) and which would otherwise generate an assignment to a literal `0`. |

## optional/

`optional/N64ModernRuntime-actionable-function-lookup.patch` turns
librecomp's bare `Failed to find function at 0x%08X` into a message that also
names the loaded section, its ROM/VRAM/size and where it was loaded. That line
is how the Cruis'n USA "pressing Start aborts" bug was diagnosed — the missing
entry points were only reachable through a function-pointer table, so the live
capture never recorded them.

It is **not** auto-applied: its hunk header was hand-written and does not carry
real context, so it needs re-fitting against the pinned `librecomp/src/overlays.cpp`
before `git apply` will take it. Do that the first time a `get_function` abort
shows up, then move it up a directory and add it to `build_stack.sh`.
