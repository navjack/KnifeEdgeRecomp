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

## Resource table at `0x800E8600`

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

Audio microcode has **not** been located yet. No `aspMain` / `n_aspMain` name
string is present, which is normal — Cruis'n USA's block was found by matching
the canonical `aspMain` prologue bytes, and the same technique applies here.
That search is the first task in `rsp/`.

## Save type

Not yet determined. The ROM contains no `eeprom`/`sram`/`flash` strings, so this
has to come from the code: EEPROM goes through SI, while SRAM and FlashRAM are
PI devices at `0x08000000`. Until it is established from a real access path,
`KNIFE_EDGE_SAVE_TYPE` stays `None` in `CMakeLists.txt` and is a documented
gate — it is not guessed.

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
