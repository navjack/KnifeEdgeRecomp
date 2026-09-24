# Knife Edge: Nose Gunner — N64 recomp workspace

Native macOS (Apple Silicon) static recompilation of *Knife Edge: Nose Gunner*
(USA, `NKEE`).

There is no public decompilation of this game, so this follows the same route
the Cruis'n workspace took: recover the metadata N64Recomp needs from the ROM
itself, then build a native app against N64ModernRuntime, RT64 and the recomp
frontend.

## Current state

**Scaffolding only.** No native binary exists yet. The full execution record and
the next gate are in [`RECOMP_STATUS.md`](RECOMP_STATUS.md), and everything read
out of the ROM so far is in [`docs/ROM_ANALYSIS.md`](docs/ROM_ANALYSIS.md).

What is established:

- the ROM is fingerprinted, and every script refuses to run against any other dump;
- the boot stub is decoded, so the entry, text, data and BSS boundaries are
  exact rather than guessed;
- the graphics microcode is identified as stock F3DEX2 2.05, which RT64 already
  supports unpatched;
- the shared upstream stack is pinned and patched reproducibly.

What is not:

- no reviewed symbol map, so no generated code;
- no audio microcode located;
- no save type determined.

## Layout

| Path | What it holds |
| --- | --- |
| `rom/` | The supported dump. Gitignored — no game data is committed. |
| `roms.toml` | Exact fingerprint manifest. |
| `splat.yaml` | ROM segmentation, derived from the boot stub. |
| `config/` | N64Recomp configurations (ELF mode, symbol-map mode, candidate probe). |
| `input/` | Drop location for the reviewed ELF or symbol map. Nothing generated. |
| `decomp/` | Splat hints and the unreviewed candidate map. |
| `generated/` | Recompiler output. Gitignored. |
| `rsp/` | Title RSP microcode and the task adapter. |
| `native/` | Shared SDL/Metal/RT64 frontend and the app's CMake module. |
| `patches/` | Reproducible upstream patches, fingerprinted in `UPSTREAM_PINS.txt`. |
| `upstream/` | Pinned checkouts. Gitignored. |
| `scripts/` | Everything below. |

## The executable path

Fetch and build the shared stack:

```bash
./scripts/bootstrap.sh && ./scripts/build_stack.sh
```

Split the ROM and produce the experimental probe:

```bash
./scripts/bootstrap_decomp.sh
./scripts/split_rom.sh
./scripts/generate_candidate_symbols.sh
```

Recompile from reviewed metadata, then build and package the app:

```bash
./scripts/recompile.sh
./scripts/build_app.sh
./scripts/verify_release_app.sh
```

`recompile.sh` stops with an actionable message when `input/` is empty. That is
deliberate: a raw ROM has bytes but no function boundaries, relocations, data
symbols or overlay information, so no placeholder game binary is ever produced.

## Runtime

The packaged bundle keeps its settings and saves under
`~/Library/Application Support/KnifeEdgeRecompiled/`, and accepts
`--rom /path/to/Knife Edge - Nose Gunner (U) [!].z64` to skip the launcher.

Development flags carried over from the shared frontend:

| Flag | Effect |
| --- | --- |
| `--rom PATH` | Install the ROM and boot past the launcher. |
| `--input-script FILE` | Drive player 1 from `<start_vi> <end_vi> <buttons_hex> [stick_x stick_y]` lines in guest VI time. |
| `--trace-output FILE` | Per-VI CSV of VI count, audio frequency, buffers and nonzero samples. |
| `--function-trace-output FILE` | Function entry/return log. Needs `trace_mode = true` in `config/recomp-symbols.toml`. |
| `--stop-after-vi N` | Quit cleanly after N guest VIs, for bounded unattended runs. |

## Pinned upstreams

Revisions and patch fingerprints are in [`UPSTREAM_PINS.txt`](UPSTREAM_PINS.txt).

- [N64Recomp](https://github.com/navjack/N64Recomp)
- [N64ModernRuntime](https://github.com/navjack/N64ModernRuntime)
- [rt64](https://github.com/navjack/rt64)
- [hlslpp](https://github.com/navjack/hlslpp)
- [libultraship](https://github.com/navjack/libultraship)
- [RecompFrontend](https://github.com/navjack/RecompFrontend)

Each recomp project builds and uses **its own** N64Recomp. Sharing another
project's binary appears to work and then fails much later, when the generated
code is compiled against a runtime API it was not built for.
