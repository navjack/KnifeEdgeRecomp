# Knife Edge: Nose Gunner — recomp workspace

Native macOS arm64 static recompilation of *Knife Edge: Nose Gunner* (USA,
`NKEE`). There is no public decompilation of this game; metadata is recovered
from the ROM. Read [`RECOMP_STATUS.md`](RECOMP_STATUS.md) first — it holds the
current gate — and [`docs/ROM_ANALYSIS.md`](docs/ROM_ANALYSIS.md) for everything
established about the ROM.

## Commands

```bash
./scripts/bootstrap.sh                   # fetch pinned upstreams
./scripts/build_stack.sh                 # patch + build N64Recomp, runtime, LUS, RT64
./scripts/bootstrap_decomp.sh            # splat venv
./scripts/split_rom.sh                   # regenerate asm/ and assets/
./scripts/generate_candidate_symbols.sh  # unreviewed probe map
./scripts/generate_rsp.sh                # RSPRecomp -> rsp/*.cpp (committed)
./scripts/recompile.sh                   # N64Recomp -> generated/
./scripts/build_app.sh                   # configure, build, package the .app
./scripts/verify_release_app.sh          # static checks on the packaged bundle
```

`scripts/common.sh` is sourced by all of them and holds the ROM path, its
SHA-256 and the shared cmake binary.

## Architecture

The build is three stages, and each refuses to proceed on missing inputs rather
than producing a placeholder:

1. **Metadata** — `input/knife-edge.elf` or `input/knife-edge.syms.toml`,
   reviewed by hand. `config/recomp.toml` and `config/recomp-symbols.toml`
   select which.
2. **Recompilation** — N64Recomp emits C into `generated/`.
   `scripts/recompile.sh` then writes `generated/recomp_decls.h`, because the
   generated code calls `<symbol>_recomp` replacements that `funcs.h` only
   partly declares.
3. **Native app** — `native/KnifeEdgeNative.cmake` links the generated code with
   N64ModernRuntime, RT64 and RecompFrontend into a bundle. Game facts (ids,
   names, ROM hash, save type) live in the top-level `CMakeLists.txt` and reach
   C++ through `native/game_config.hpp.in`.

`native/main.cpp` is the shared frontend: SDL window, Metal view for RT64,
audio resampling to 48 kHz, input, and the development flags documented in
`README.md`. It is game-agnostic apart from the `game_config.hpp` macros and
`set_default_bindings()`.

## Conventions

- **Never invent game metadata.** If the recompiler lacks a symbol map, the
  correct outcome is an actionable error, not a placeholder binary. The
  candidate map in `decomp/` is a probe; it is gitignored and never promoted to
  `input/` without review.
- **Never share another project's N64Recomp binary.** Build this repo's own.
  Mismatches surface much later as compile errors in generated code.
  Regenerate after `rm -rf generated/` so stale files do not survive.
- **Patches are reproducible.** Add one to `patches/`, fingerprint it in
  `UPSTREAM_PINS.txt`, and apply it from `build_stack.sh` behind a marker-string
  guard so re-running is a no-op.
- **Commit early and often.** The git history here exists to revert breaking
  changes; prefer small commits over large ones.
- **Claim only what was demonstrated.** Distinguish "the process starts" from
  "the game renders" from "the game is playable". Say which one happened.
- The remaining standing rules — bundle layout, codesign, rpath stripping,
  soak-testing — are in `RECOMP_STATUS.md` under "Standing rules".

## Do not commit

Game data (`rom/`, any `*.z64`), upstream checkouts, build output and generated
code are all gitignored. Keep it that way.
