# gnw-doom

A [retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd) homebrew app of
DOOM I & II for the Nintendo Game & Watch (STM32H7B0), based on
[rp2040-doom](https://github.com/kilograham/rp2040-doom) /
[Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom).

Originally based on [rota1001/stm32h7-baremetal-doom](https://github.com/rota1001/stm32h7-baremetal-doom)
and [doomgeneric](https://github.com/ozkl/doomgeneric) (see the `doomgeneric`
branch), later pivoted to rp2040-doom for its BSP-level loading and in-place
compression — yielding an XIP-able, compressed binary on flash.

The engine lives in the `rp2040-doom/` submodule (fork branch `gnw-stm32h7b0` =
upstream `rp2` + one minimal engine-hooks commit); everything G&W-specific —
the `src/gnw/` platform layer, `config.h`, `linker.ld`, the build — lives in
this repo. The payload is a relocatable XIP blob that binds to a retro-go-style
firmware ABI table at runtime (test firmware: the `retro-go-porting-toolkit/`
submodule).

## Building

Prerequisites:

- `arm-none-eabi-gcc` (a recent Arm GNU toolchain; builds both the payload and
  the test firmware)
- host `gcc`/`g++` (builds `whd_gen`, the WAD→WHD/WHX converter, from the
  engine submodule)
- `python3` + `pip install -r requirements.txt` (gnwmanager for flashing/SWD,
  pyelftools for the relocation-table build step)
- `git submodule update --init rp2040-doom` (the engine)

```
make                      # doom: doom1.wad if present, else doom1-shareware.wad
make WAD=doom2.wad        # DOOM II -> build/doom2.bin
make build-firmware       # the test firmware (retro-go-porting-toolkit)
make flash                # build + flash BOTH (firmware -> bank2, doom -> ext flash)
make start-app            # boot the firmware (gnwmanager start bank2)
```

One IWAD per invocation; build DOOM I and DOOM II by invoking make twice.

The test firmware needs the toolkit submodule, which is marked `update = none`
(not needed when this repo is consumed as a submodule):
`git submodule update --init --checkout retro-go-porting-toolkit`. It has no
dependencies beyond the same Arm toolchain — its `deps/` vendors the ST HAL,
CMSIS and littlefs.

## WAD files

- `doom1-shareware.wad` (tracked): the **shareware** IWAD — the one WAD id
  Software permits redistributing. Default when no `doom1.wad` is present.
- `doom1.wad` / `doom2.wad` (repo root, **never tracked** — `.gitignore` blocks
  all other WADs): place legally-owned full IWADs here. Any IWAD works via
  `WAD=<file>`; files are recognized by sha1 and the build warns when a WAD is
  not in the tested-working list. Unity re-release WADs (widescreen art) are
  handled automatically (cropped at conversion time).

## Memory map (RAM-overlay model)

The payload is a retro-go homebrew overlay: the launcher copies `doom.bin` to
`__RAM_EMU_START__` and jumps to the `GWHB` stage-1, which unpacks every
segment to its final address — **no code executes from external flash and
nothing is relocated**. The WHD game data is a separate file mapped at runtime
through the firmware ABI. Numbers below are the Ultimate DOOM build (doom2 is
identical apart from the WHD). Tunables (all linker-`ASSERT`ed): the ITCM/DTCM
object lists in `linker.ld`, `PATCH_CACHE_BYTES` and `TEXT_AXIS_ORIGIN`.

```
══════════════════ ITCM 64K @ 0x00000000 — zero-wait code ══════════════════
0x00000000 ┌────────────────────────────────────────────┐
           │ (256 B reserved: optional fw null-guard)    │
0x00000100 ├────────────────────────────────────────────┤
           │ .itcram_hot                       58.6K     │  hot-fn list +
           │   R_Render*, pd_render, p_map,              │  pd_render/p_map/
           │   p_enemy, p_sight, p_maputl                │  p_enemy/p_sight…
0x0000EB40 ├────────────────────────────────────────────┤
           │ free                               5.2K     │  ← ITCM headroom
0x00010000 └────────────────────────────────────────────┘

══════════════ DTCM 128K @ 0x20000000 — FIRMWARE-OWNED, doom: 0 ═════════════
0x20000000 ┌────────────────────────────────────────────┐
           │ fw .data + .bss                  ~16.6K     │  logbuf, HAL state…
0x200040C0 ├────────────────────────────────────────────┤
           │ fw malloc heap                      85K     │  FatFS/LFS/dialogs;
           │                                             │  (apps: dtcm_malloc)
0x200194C0 ├────────────────────────────────────────────┤
           │ padding / redzone (256 B)         ~6.9K     │  MPU region 2 guard
0x2001B000 ├────────────────────────────────────────────┤
           │ fw stack (doom runs on it)          20K     │  SP ↓ from 0x2001FFF0
0x20020000 └────────────────────────────────────────────┘

═══════════ AXISRAM 1M @ 0x24000000 — app territory after launch ════════════
0x24000000 ┌────────────────────────────────────────────┐
           │ LUT8 framebuffers  2 × 75K         150K     │  UNCACHED (fw MPU
           │ (+4K LTDC slack to 0x26800)                 │  regions 3–6)
0x24028000 ├────────────────────────────────────────────┤
           │ .pcache  patch/texture cache       140K     │  was 320K; perf knob
0x2404B000 ├────────────────────────────────────────────┤ ← __RAM_EMU_START__
           │ .gwhb  GWHB magic + stage-1       0.3K      │  image loads here,
           │ .bss   (overlaps consumed image)   288K     │  bss zeroed after
0x24093548 ├────────────────────────────────────────────┤  segments copied out
           │ zone heap (z_zone)               203.4K     │  obs. peak ~210K
0x240C5000 ├────────────────────────────────────────────┤ ← TEXT_AXIS_ORIGIN
           │ .text_axis  cold code + rodata   228.7K     │  (7.5K slack)
0x24100000 └────────────────────────────────────────────┘

═══════ AHBRAM 128K @ 0x30000000 — D2 SRAM, doom's "fast data" tier ═════════
0x30000000 ┌────────────────────────────────────────────┐
           │ fw audio DMA ring + .ahb head     ~6.4K     │  UNCACHED (fw rgn 0;
           │ (rest of 16K subregion unused)              │  doom rgn 7 skips it)
0x30004000 ├────────────────────────────────────────────┤ ← doom MPU region 7:
           │ .dtcm_bss  render scratch          86.1K    │   cacheable WBWA
0x30019864 │ .data      initialized globals      2.5K    │   (survives the fw's
0x3001A268 │ .text_dtcm warm code tier          19.6K    │   LUT8 MPU rewrite)
0x3001F0D8 ├────────────────────────────────────────────┤
           │ free                                3.8K    │
0x30020000 └────────────────────────────────────────────┘

═════════════════ External flash (XIP @ 0x90000000) ═════════════════════════
  Real retro-go (SD_CARD=0 example, 64M chip):
    FrogFS @ +0      (roms/bios/covers; doom.bin 317K, doom.whd 6.9M,
                      doom2.whd 7.9M) · littlefs @ +54M (10M)
  Test firmware:     doom.bin @ EXTFLASH_OFFSET · <name>.whd @ +768K

  intflash: retro-go firmware · ABI table @ VTOR+0x400 (gw_firmware_abi_t)
  Image file = 317K (gwhb+itcm+data+text_dtcm+text_axis LMAs chained at
  0x2404B000); stage-1 unpacks ITCM/AHB/AXITEXT, then .bss zeroing retires it.
```

## Repo structure (for devs)

```
Makefile.common      user-facing stages & variables (start here)
Makefile             build machinery: WAD classification, engine/platform
                     compile rules, whd_gen host build, dual-link reloc,
                     test-firmware build, gnwmanager flash targets
config.h             stand-in for rp2040-doom's CMake-generated config.h
linker.ld            payload linker script (XIP from extflash, ITCM hot set,
                     .reloc_hdr at image offset 0)
src/gnw/             the G&W platform layer: video/sound/input/timer/system
                     backends, ABI binding (rg_abi.h, abi_stubs.c, rg_data.h),
                     perf overlay, persistence, relocation header, fast mem,
                     pico-SDK compat shims (compat/)
scripts/build/       wad_plan.py (sha1 IWAD classifier), wadwide.py (Unity
                     widescreen crop), gen_reloc_table.py (dual-link diff ->
                     relocation table appended to the blob)
scripts/debug/       doom-specific SWD tools: tracepull.py (TRACE=1 pipeline
                     timing), screenshot.py (framebuffer+CLUT dump)
rp2040-doom/         the engine (submodule): fork branch gnw-stm32h7b0 =
                     upstream rp2 + one minimal engine-hooks commit (~22 files)
retro-go-porting-toolkit/
                     the test firmware (submodule, update=none): minimal
                     retro-go API surface published as an ABI table at
                     VTOR+0x400; owns deps/ (ST HAL, CMSIS, littlefs) and the
                     generic SWD debug tools (scripts/debug/)
```

How it fits together: the build compiles the engine out of `rp2040-doom/`
against `src/gnw/` and links it twice (1 MB apart); `gen_reloc_table.py` diffs
the two images to derive the exact absolute-pointer relocation table, which is
appended to the blob behind the `.reloc_hdr`. The firmware finds the blob on
external flash by header magic, relocates it in place to its install address,
and jumps in; the blob resolves every firmware service at runtime through the
`gw_firmware_abi_t` table at VTOR+0x400 — no link-time coupling in either
direction. Replacing the test firmware with real retro-go is a consumer-side
change only (see the toolkit README).

## License

This project is licensed under **GPLv2**. The shareware WAD remains the
property of id Software and is distributed under its original shareware terms.
