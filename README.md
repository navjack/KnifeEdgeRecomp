# Knife Edge: Nose Gunner — N64 recomp workspace

Native macOS (Apple Silicon) static recompilation of *Knife Edge: Nose Gunner*
(USA, `NKEE`).

There is no public decompilation of this game, so this follows the same route
the Cruis'n workspace took: recover the metadata N64Recomp needs from the ROM
itself, then build a native app against N64ModernRuntime, RT64 and the recomp
frontend.

## No game data

This repository contains no ROM, no game code and no game assets. What is
committed is the port's own code, the build tooling, and factual metadata
recovered from the ROM: segment boundaries, function addresses and sizes.
Everything derived from the game itself -- the recompiled C in `generated/`, the
recompiled audio microcode, the split disassembly -- is produced on your machine
from your own dump of the USA cartridge, whose SHA-256 is checked before
anything runs.

## Current state

**Playable.** The packaged app boots straight into the game, and stage 1 plays
through its boss with a gamepad and rumble. Presentation is the original's
(4:3, no enhancements). Pacing matches a real console to within about 2%
across the attract loop and stage 1. See [`docs/PACING.md`](docs/PACING.md).
The execution record is in [`RECOMP_STATUS.md`](RECOMP_STATUS.md), and what was
read out of the ROM is in [`docs/ROM_ANALYSIS.md`](docs/ROM_ANALYSIS.md).

What is not yet established:

- the symbol map is still the unreviewed candidate (`decomp/`), not a reviewed
  `input/` map;
- stages after the first, and every boss beyond stage 1's, have not been
  checked against hardware;
- no save type is determined (`SAVE_TYPE` is `None`).

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
| `--launcher` | Keep the launcher up instead of starting the game. |

Environment variables, all for development:

| Variable | Effect |
| --- | --- |
| `KNIFE_EDGE_RDP_VIS=N` | Fixed pacing, N retraces a frame, in place of the hardware model. |
| `KNIFE_EDGE_RDP_MODEL=0` / `measure` | Turn the pacing model off, or record without pacing. |
| `KNIFE_EDGE_RDP_MODEL_CSV=FILE` | Per-frame raw counts: triangles, pixels by mode, bytes, spans, CPU. |
| `KNIFE_EDGE_MODEL_CPI` / `_BW` / `_SPAN` | Override the fitted CPU and RDP costs. |
| `KNIFE_EDGE_MODEL_TRACE=N` | Print graphics task N's rectangles and image settings. |
| `KNIFE_EDGE_COMM_LOG=1` | Transmission states, sound calls and frame-callback changes, timestamped. |
| `KNIFE_EDGE_AUDIO_LOG=FILE` / `KNIFE_EDGE_DUMP_AUDIO=FILE` | Per-buffer audio queue log, raw guest audio. |
| `KNIFE_EDGE_DUMP_RDRAM=DIR:N,...` | Guest-order RDRAM at the given graphics tasks. |
| `KNIFE_EDGE_POKE=ADDR=VALUE,...` | Rewrite guest words every VI (test harness, e.g. pin health). |
| `KNIFE_EDGE_SCRIPT_UNTIL_LEVEL=1` | Stop `--input-script` input once a level starts. |
| `KNIFE_EDGE_SESSION_LOG=1` | Log to `~/Library/Logs/KnifeEdge/` with a hang sampler (on by default from Finder). |

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

## License

GPL-3.0 (see [`LICENSE`](LICENSE)), matching N64ModernRuntime, which the app
links. The bundled fonts keep their own licenses: Inter, Noto Emoji and
PromptFont are all under the SIL Open Font License 1.1.

*Knife Edge: Nose Gunner* belongs to its rights holders. This project is not
affiliated with or endorsed by them.
