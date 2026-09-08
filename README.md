# nesrecomp-ds

A Nintendo DS runner for [nesrecomp](https://github.com/mstan/nesrecomp).

Statically recompiled 6502 code runs as native ARM on the DS, and the NES PPU
is mapped onto the DS's own 2D hardware rather than software-rendered. Full
speed on original DS hardware.

Validated with
[SuperMarioBrosNESRecomp](https://github.com/mstan/SuperMarioBrosNESRecomp):
four worlds played on a DSpico flashcart with no graphical glitches.

| | |
|---|---|
| Binary size | 2.6 MB (ARM Thumb) |
| Frame time | 4–9 ms of a 16.7 ms budget |
| Frame rate | 60.0988 Hz, matching NTSC NES |
| Video | Hardware BG + OAM, status bar on the sub screen |
| Audio | NES pulse / triangle / noise on DS PSG channels |
| Save states | R+X saves, R+Y loads |

## Layout

This runner is not standalone — it compiles the C that nesrecomp generates for
a specific game. Clone it *inside* a game project:

```
SuperMarioBrosNESRecomp/
  generated/            recompiled C
  nesrecomp/            submodule
  nesrecomp-ds/         this repo
```

Or point it elsewhere with `make GAME_DIR=../path/to/game-project`.
`GAME_DIR` must be relative — the devkitARM template prefixes source and
include paths with `$(CURDIR)`.

## Building

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `nds-dev`:

```
sudo dkp-pacman -S nds-dev
```

then:

```
git clone https://github.com/mstan/SuperMarioBrosNESRecomp
cd SuperMarioBrosNESRecomp && ./setup.sh
git clone https://github.com/the-marmolade/nesrecomp-ds
cd nesrecomp-ds
bash tools/fetch_deps.sh    # copies mapper.c from the nesrecomp submodule
make
```

## Supplying the ROM

No ROM is included. Use your own dump of a game you own, named `smb.nes`:

- **SD card** — put it in the root of your flashcart's SD card.
- **Embedded** — put it in `nitrofiles/` before `make`, which builds it into
  the binary and avoids needing DLDI. Don't redistribute a binary built this
  way; it contains recompiled game code.

## Running

**Hardware:** copy the `.nds` to your flashcart and launch it. Tested on
DSpico with Pico Launcher.

**Emulator:** melonDS. Enable DLDI under Config → Emu settings.
DeSmuME does *not* work — it predates the calico-based libnds and shows a
white screen.

## Controls

| DS | NES |
|---|---|
| D-pad, A, B, Start, Select | as labelled |
| L | fps / debug overlay |
| R + X / R + Y | save / load state |

## Adding a game

Most of the runner is game-independent. Four things aren't, and they live in
`include/game_config.h`:

```c
static const GameConfig SMB = {
    .name          = "Super Mario Bros.",
    .top_line      = 32,     // first NES scanline on the top screen
    .hud_lines     = 32,     // status bar height, 0 if the game has none
    .hud_nametable = 0,      // which nametable the HUD is drawn into
    .sprite0_cycle = 5910,   // 2273 + scanline * 113.67
};
```

Mirroring is read from the iNES header via the mapper, not configured here.

**Another NROM game should mostly be a config entry.** Mapper support beyond
NROM is wired (PRG reads go through `mapper_peek_prg`, writes reach
`mapper_write`) but untested, and MMC3 additionally needs scanline IRQs, which
this runtime has no concept of yet.

## How it works

- **Backgrounds.** NES nametables are written into a 512×256 DS text
  background — exactly two nametables side by side, which is the right shape
  for vertical mirroring. CHR converts 2bpp planar → 4bpp linear once at load.
- **Sprites.** The NES's 64 map 1:1 onto DS OAM, which holds 128.
- **240 vs 192 lines.** Rather than scaling, the playfield goes on the top
  screen unscaled and the status bar on the bottom. No filtering, no dropped
  scanlines.
- **Audio.** NES pulse duty cycles land almost 1:1 on DS PSG channels; the
  triangle plays a 32-step wavetable as a looping sample, which is what the
  NES triangle physically is.

Everything expensive happens outside vblank into RAM shadow buffers, DMA'd to
VRAM during vblank. Building directly into VRAM overruns the ~1.3 ms window
and tears.

The DS panel refreshes at 59.8261 Hz against the NES's 60.0988 Hz, so one
extra NES frame is emulated every ~219 frames to keep game time accurate.

## Notes for anyone extending this

Two things the recompiler expects that are easy to miss:

- **The runner must push the 6502 NMI hardware frame** (PC high, PC low,
  status) before calling `func_NMI()`, because the generated handler ends in
  `RTI`, which pops all three. Without it the stack pointer climbs 3 bytes per
  frame until it overwrites something that matters — which shows up as
  seemingly unrelated graphical corruption.
- **`nes_interp_dispatch` is stubbed.** The recompiler normally inlines jump
  tables at call sites, but anything reaching `JumpEngine` indirectly falls
  through to the interpreter, which this build does not include.

`call_by_address_tail` also lacks upstream's JMP-cycle flattening, so a tail
loop will grow the C stack. Not hit in normal SMB play, but latent.

## Licence

MIT. Contains no Nintendo code or assets.
