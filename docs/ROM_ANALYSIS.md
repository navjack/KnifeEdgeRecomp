# Knife Edge: Nose Gunner — ROM analysis

Everything here is read directly out of the supported dump
(`rom/Knife Edge - Nose Gunner (U) [!].z64`,
sha1 `b247167e37e7f62924be6b0d2362a091fd2352ac`). Nothing is assumed from
another title.

## Cartridge header

| Field | Offset | Value |
| --- | --- | --- |
| PI BSD config / endianness | `0x00` | `80 37 12 40` (big-endian `.z64`) |
| Clock rate | `0x04` | `0x0000000F` |
| Boot address | `0x08` | **`0x800C2400`** |
| libultra release | `0x0C` | `0x00001449` |
| CRC1 | `0x10` | `0xFCE0D799` |
| CRC2 | `0x14` | `0x65316C54` |
| Internal name | `0x20` | `KNIFE EDGE` |
| Cartridge ID | `0x3B` | `NKEE` (media `N`, code `KE`, region `E`/USA) |
| Size | — | 8 MiB (`0x800000`) |

`rom_hash` for `recomp::GameEntry` is XXH3-64 of the whole file:
`56605501a85c79af`. (Verified method: the same computation reproduces Cruis'n
USA's checked-in `ef2918d8cdc30441`.)

## Memory layout, derived from the boot stub

The stub at ROM `0x1000` / VRAM `0x800C2400` is 14 instructions:

```
800C2400  lui   t0, 0x800F      ; t0 = 0x800EEF50  (bss start)
800C2404  lui   t1, 0x0007
800C2408  addiu t0, t0, -4272
800C240C  ori   t1, t1, 0xBCA0  ; t1 = 0x0007BCA0  (bss size, counted down by 8)
800C2410  addi  t1, t1, -8      ; <- loop top
800C2414  sw    zero, 0(t0)
800C2418  sw    zero, 4(t0)
800C241C  bne   t1, zero, 800C2410
800C2420  addi  t0, t0, 8
800C2424  lui   t2, 0x800D
800C2428  lui   sp, 0x8012
800C242C  addiu t2, t2, -10576  ; t2 = 0x800CD6B0
800C2430  jr    t2               ; boot continues at 0x800CD6B0
800C2434  addiu sp, sp, 0x6A60   ; sp = 0x80126A60
```

So:

| Region | ROM | VRAM | Size |
| --- | --- | --- | --- |
| entry stub | `0x1000`–`0x1038` | `0x800C2400`–`0x800C2438` | `0x38` (padded to `0x1050`) |
| text | `0x1050`–`0x26FF0` | `0x800C2450`–`0x800E83F0` | `0x25FA0` |
| embedded data blob | `0x26FF0`–`0x27200` | `0x800E83F0`–`0x800E8600` | `0x210` |
| data / rodata | `0x27200`–`0x2DB50` | `0x800E8600`–`0x800EEF50` | `0x6950` |
| bss (cleared by the stub) | — | `0x800EEF50`–`0x8016ABF0` | `0x7BCA0` |
| bulk game data | `0x2DB50`–`0x800000` | loaded on demand | `0x7D24B0` |
| initial stack pointer | — | `0x80126A60` | — |
| real boot function | — | `0x800CD6B0` | — |

Splat independently confirms every one of these: it resolves the stub's
operands to `D_800EEF50`, `0x7BCA0`, `func_800CD6B0` and `D_80126A60` without
being told any of them.

The last real function is `func_800E83CC`, ending at ROM `0x26FEC`. The
`0x26FF0`–`0x27200` window is **not** text: Splat classifies it as a data blob
(`D_800E83F0`) and the instructions immediately before it are flagged
handwritten (`mtc0 $s3, $3`). Its contents decode as IMEM-relative jumps, so it
is most likely a handwritten RSP routine embedded in the code segment. Treat it
as data until proven otherwise; a candidate map that takes it for text will
generate junk.

ROM `0x27200` then begins the pointer table below.

## Overlay layout

The `0x800E8600` table is one descriptor among many. Four loaders consume them,
and between them they pin down the struct:

| Loader | Reads | Does | Descriptors actually handed to it |
| --- | --- | --- | --- |
| `func_800C3968` | `[0]`,`[1]`,`[2]` | DMA only, 3-word descriptor | — |
| `func_800C3930` | `[0]`,`[2]`,`[3]` | DMA only | 22 references, **all data** |
| `func_800C38C8` | `+0x00..0x1C` | writeback dcache, inval dcache, DMA | 21 references, **all code** |
| `func_800C3848` | `+0x00..0x24` | the above plus inval icache | **none — dead code** |

```c
struct OverlayDesc {      // 0x28 bytes
    void* dest;           // 0x00  destination in RDRAM
    void* destEnd;        // 0x04  end of destination (text + bss)
    u32   romStart;       // 0x08
    u32   romEnd;         // 0x0C
    void* writebackStart; // 0x10  -> osWritebackDCache
    void* writebackEnd;   // 0x14
    void* invalStart;     // 0x18  -> osInvalDCache
    void* invalEnd;       // 0x1C
    void* icacheStart;    // 0x20  -> osInvalICache   (code overlays only)
    void* icacheEnd;      // 0x24
};
```

All of them DMA through `func_800D1D10(romStart, dest, size)`.

An earlier reading of this table took the instruction-cache invalidate in
`func_800C3848` to be the code/data discriminator. **That was wrong.**
`func_800C3848` is never referenced at all, while `func_800C38C8` — which does
*not* invalidate the icache — receives all 21 code descriptors and
`func_800C3930` all 22 data ones. The sound evidence is the per-descriptor
classification in `tools/overlay-map/overlay_map.py`, by MIPS prologue and
`jr $ra` density; the loader split independently confirms it with a perfect
21/0 and 0/22 separation.

(The 21-vs-17 gap is descriptors referenced from more than one call site, e.g.
`D_800EB4A0` from both `func_800CBA00` and `func_800CC8D0`. Distinct code
descriptors reachable from the resident segment: 11 — see below.)

**36 descriptors, 17 of them code.** Every code descriptor is immediately
followed by a data one at the contiguous destination — classic `{text, data}`
module pairs. Five destinations are reused by more than one overlay, which is
why they must be recompiled as relocatable sections rather than loaded once:

| Swap point | Overlays |
| --- | --- |
| `0x8016D6F0` | 2 — a 20 KB boot module, and the 205 KB gameplay module at ROM `0x1501A0` |
| `0x8017B0C0` | 8 |
| `0x801D21F0` | 6 |
| `0x801DC320` | 2 |
| `0x80224140` | 2 |

The resident segment at `0x800C2400` is therefore only the engine half. All six
call targets that the first recompiler probe could not resolve
(`0x801752D8`, `0x80178044`, `0x8017B678`, `0x8017BC34`, `0x80193B40`,
`0x80196810`) fall inside the 205 KB module at `0x8016D6F0`.

### Two tiers

Of the 17 code overlays, only **11** are loaded from the resident segment, and
all 11 agree exactly with `overlay_map.py`. The remaining six — ROM `0x1B66F0`,
`0x255660`, `0x34B590`, `0x3EFDD0`, `0x497140`, `0x4DC6D0` — are referenced from
no resident call site and *all* target `0x801D21F0`. They are loaded by the
gameplay overlay itself, which makes `0x801D21F0` the level/stage swap point and
the overlay system two-tier:

```
resident 0x800C2400
  └─ gameplay overlay  0x8016D6F0   (ROM 0x1501A0, 205 KB)
       └─ level modules 0x801D21F0  (6 of them, swapped per stage)
```

`map-recovery locate` reports **0 external jal targets**, so no further overlay
is hiding in unmapped ROM — the 17 account for every call in the image.

Run `tools/overlay-map/overlay_map.py <rom>` for the full table.

## The descriptor at `0x800E8600`

The first words of the data segment are a ROM→RDRAM load table:

```
800E8600  8016ABF0   ; destination, == end of bss
800E8604  8016D6F0
800E8608  0002DB50   ; source ROM offset, == end of the boot segment
800E860C  0002FC30
```

The destination of the first entry is exactly where BSS ends, and its source is
exactly where the boot segment ends. This is the table to decode when mapping
the game's streamed data and any relocatable code; it is the starting point for
populating `config/relocatable-sections.txt`.

ROM `0x2DB50` itself begins with `DC 08 00 08 80 16 AE C0` — an F3DEX2
`G_MOVEMEM` — so the region immediately after the boot segment is display-list
and graphics data.

## Microcode

Graphics, identified by the strings embedded in the boot segment:

| ROM | String | RT64 instance |
| --- | --- | --- |
| `0x02D458` | `RSP Gfx ucode F3DEX       fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.` | `F3DEX2.fifo 2.05` |
| `0x02D878` | `RSP Gfx ucode F3DLX.Rej   fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.` | `F3DLX2.fifo 2.05.Rej` |

Despite the `F3DEX` spelling, the padded `fifo 2.0x` / `1998` form is F3DEX**2**.
Both revisions are already in RT64's hash table (`src/gbi/rt64_gbi.cpp`), so no
GBI patch is expected — unlike Cruis'n Exotica, which needed one for an
unmapped 2.08 build. Confirm from RT64's own identification at runtime rather
than from this table.

Audio microcode has **not** been positively identified, but the block it lives
in has been found. ROM `0x23DF0`–`0x27200` (13 KB) sits inside the resident
segment and is RSP microcode, not CPU code:

| Region | COP2 | LWC2/SWC2 | MIPS prologues | `jr $ra` |
| --- | --- | --- | --- | --- |
| CPU text `0x1050`–`0x23DF0` | **0** | **0** | 362 | 606 |
| `0x23DF0`–`0x27200` | **895** | **506** | **0** | 14 |

Zero vector instructions anywhere in the real CPU text against 1,401 in that
window is unambiguous. Splat disassembles it as MIPS unless told otherwise and
invents functions like `func_800E51F0`, an 8-byte `j 0x84001064` — a jump
straight into RSP IMEM. `splat.yaml` now marks it `bin`.

## Save type

Narrowed, not settled. Enumerating every hardware register Splat symbolised
gives a complete picture of what the game touches:

- SP, MI, VI, AI, PI and SI are all present.
- There is **no `D_A8xxxxxx` anywhere**, so the game never touches cartridge
  domain 2 at `0x08000000`. **SRAM and FlashRAM are both ruled out.**
- `func_800E0DF0` is `__osSiRawStartDma` (SI_STATUS busy test, SI_DRAM_ADDR,
  then PIF_ADDR_RD64B or WR64B against `0x1FC007C0`). It is the only function
  that touches PIF RAM.
- `func_800E1150` and `func_800E3350` shift addresses by 3 and by 5 — 32-byte
  pages — which is **Controller Pak**, not EEPROM's 8-byte blocks.

So the device is EEPROM or nothing; EEPROM could not be confirmed from statics.
`KNIFE_EDGE_SAVE_TYPE` stays `None` in `CMakeLists.txt` rather than guessed.

## Open questions

1. Where is the audio microcode block, and which revision is it?
2. What is the full meaning of the `0x800E8600` table — how many entries, and
   does any entry carry code rather than data?
3. Which save device does the game use?
4. What exactly is the `0x26FF0`–`0x27200` blob (`D_800E83F0`)? It looks like a
   handwritten RSP routine. If it is, it needs an entry in `rsp/` rather than
   a place in the CPU symbol map.

## Splat results, first split (2026-09-24)

`scripts/split_rom.sh` with the committed `splat.yaml`:

| Category | Size | Share |
| --- | --- | --- |
| `asm` (main text) | 156 KB | 1.86% |
| `bin` (IPL3) | 4 KB | 0.05% |
| `hasm` (entry stub) | 80 B | 0.00% |
| `header` | 64 B | 0.00% |
| unknown (bulk game data past `0x2DB50`) | 8 MB | 97.77% |

**566 functions** in the main segment, plus 23 embedded data blobs and 10
`dlabel` blocks. That the code is under 2% of the cartridge is expected for a
rail shooter: nearly all of the 8 MB is streamed art, audio and level data
reached through the `0x800E8600` table.
