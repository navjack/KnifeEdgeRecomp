# Pacing: how fast Knife Edge runs, and how the port reproduces it

Knife Edge is known for running too fast in every emulator. This is why, and
how the port gets the speed of a real console, checked against footage of a
real console.

## How the game paces itself

- **A step runs from the retrace callback, and only when no graphics task is
  outstanding.** `func_800D2930` hands each retrace to the current frame
  callback, along with the pending task count. Every callback opens with
  `bgtz pending, skip`. The count drops the moment the task completes
  (`func_800D1730`), not at a retrace. So one frame is the CPU's step, then the
  RCP drawing it, one after the other, rounded up to the next retrace.
- **Nothing is scaled by elapsed time.** The skip branch does nothing: no
  retrace counter, no delta. Each step moves the world a fixed amount. The
  game's speed is therefore exactly the console's frame rate.
- **Emulators run it fast** because their RDP finishes instantly, so the
  pending count is always zero by the next retrace.
- **Transmission boxes have a real-time floor.** `func_801978C0` busy-waits
  until `osGetTime()` reaches 2,800,000 ticks after `osSetTime(0)`. This is the
  only place the game reads a clock. libultra rebases time only at retraces, so
  on hardware this locks each step to exactly 3 retraces (50 ms). See
  `native/game/clock.cpp`; ultramodern's `osSetTime` gave 59.7 ms instead.

## The model (`native/game/rdp_model.cpp`, `native/game/pipeline_trace.cpp`)

Each display list is walked as F3DEX2 would process it, and the frame is paced
by the retraces its cost needs:

    frame = max(CPU step, host time) + max(RSP, RDP)

| Part | Source |
|---|---|
| CPU step | Guest instructions the step's own thread ran (`__thread` counter), times the CPI |
| Host time | Real time the step took. Only the transmission's clock wait makes it matter. |
| RSP | F3DEX2 cycle counts measured by the F3DEX3 project: dispatch 12; vertex pair 54 unlit, 73–118 lit; light transform; triangle drawn 172, offscreen 27, clipped 32, backface 38, degenerate 42 |
| RDP | Each pixel batch costs its pipeline rate (1-cycle 1, 2-cycle 2, fill/copy 0.25) or its RDRAM bytes at the effective bandwidth, whichever is slower. Plus texture loads at that bandwidth, a cost per scanline span, 30 cycles of setup per triangle, and the documented sync waits. The VI's framebuffer scanout comes out of the bandwidth. |

Three values are fitted to hardware, not taken from documents:

| Value | Fitted | Meaning |
|---|---|---|
| CPU cycles per instruction | 4.5 | The VR4300 stalls on every cache miss to RDRAM |
| Raw RDP bandwidth | 1.47 B/cycle (92 MB/s) | About 16% of RDRAM's 562.5 MB/s peak, before VI scanout |
| Span cost | 36 cycles | The RDRAM access latency per scanline |

## The fit

The source is a hardware longplay (YouTube `1aDxy6BKbJ0`), 1280×720 at 30 fps.
Its marker times are in `tools/pacing-fit/longplay_markers.json`.

- **Stage 1, between its two transmissions.** Step counts are fixed on the
  rail, so the port logs 4,404 steps between the transmissions in every run,
  firing or not. The footage covers the same stretch in 166.70 s. Two pauses
  (3.33 s and 3.17 s) show as dark, still pause screens, which leaves 160.20 s
  of stepping. As a cross-check, the footage has 4,399 distinct frames in that
  time. At 30 fps from a 60 Hz console, every step of two or more retraces shows
  exactly once. So the console ran 27.5 steps a second: about 30 in light
  stretches, 22 through the mini-boss.
- **Title and story.** The attract loop plays untouched in both. The title is
  362 steps and takes 31.45 s on hardware. It is drawn at 640×480, and its
  still screen stepped 12 times a second. The story is 3,818 steps in 138.55 s.

`tools/pacing-fit/fit.py` recomputes every recorded frame's cost from its raw
counts for each candidate. It sums whole retraces per stretch and minimises the
RMS relative error over the title, the story and 14 gameplay windows of 300
steps. The result is 4.3%. The worst gameplay windows are within 10%, and the
title and story within 2.3%.

End to end, with default settings:

| Stretch | Port | Hardware |
|---|---|---|
| Title | 32.15 s | 31.45 s |
| Story | 139.63 s | 138.55 s |
| Stage 1 opening transmission | 36.53 s | 36.35 s |
| Stage 1 between transmissions | 159.18 s | 160.20 s |

What the fit rules out:
- A fixed 30 fps runs the gameplay stretch 9% fast.
- Documented peak rates alone predicted 60 fps.
- Only the CPU slower would need 14 cycles per instruction.
- Reading only one field's lines for the 640×480 VI makes the title 15% fast.

## Reproducing it

```bash
tools/pacing-fit/extract_hw.sh longplay_gameplay.mp4 hw.txt
KNIFE_EDGE_RDP_MODEL=measure KNIFE_EDGE_RDP_VIS=1 KNIFE_EDGE_RDP_MODEL_CSV=intro.csv \
  "Knife Edge Nose Gunner.app/Contents/MacOS/knife_edge" --stop-after-vi 6000
KNIFE_EDGE_RDP_MODEL=measure KNIFE_EDGE_RDP_VIS=1 KNIFE_EDGE_RDP_MODEL_CSV=play.csv \
  KNIFE_EDGE_POKE=0x8011D464=100 KNIFE_EDGE_SCRIPT_UNTIL_LEVEL=1 KNIFE_EDGE_COMM_LOG=1 \
  "Knife Edge Nose Gunner.app/Contents/MacOS/knife_edge" --input-script mash.txt --stop-after-vi 13000
tools/pacing-fit/fit.py intro.csv play.csv hw.txt <gfx of the first step after the opening transmission closes>
```

- `mash.txt` alternates START and A every 75 VIs through the menus.
- `KNIFE_EDGE_SCRIPT_UNTIL_LEVEL` stops the script once gameplay's frame
  callback is installed.
- `KNIFE_EDGE_COMM_LOG` prints the transmissions' open and close, which gives
  the first gfx.
- Pinning health at `0x8011D464` keeps an unattended ship alive to the second
  transmission.

## Open

- Only stage 1 and the attract loop are fitted. Later stages, boss fights and
  menus are predicted, not checked.
- The per-window error, 10% at worst, may come from what the player shot on
  the console; the port's recordings fire the vulcan but not the specials.
- Nothing here was measured on a console directly. A test ROM timing the RDP's
  Z-buffered and anti-aliased fill would replace the fitted bandwidth with a
  measured one.
