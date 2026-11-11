# Stumped for the NES (plan)

**Status:** an idea for later, not started. Like the Game Boy and C64 versions it's a rewrite, not a
port of `src/main.c`: the puzzles, art and music ideas carry over, the code does not. Roughly the same
amount of work as the Game Boy version, and much of it can be shared (see [Shared core](#shared-core)).

## Why it suits the NES

- **Squares:** the NES colours its background in 16 x 16 blocks, exactly the size of one square in our
  art. Each square can pick its own 3-colour palette (plus the shared background colour), so hedges,
  floor, spots and acorns all get their own colours with no colour clash.
- **Sound:** two pulse channels, a triangle and noise match the music's parts closely: triangle for the
  bass, noise for the kick, snare and shaker, the pulses for arpeggios and the melody. The DMC sample
  channel can add a real kick or a soft "pop" for pushes.
- **Saves:** a cartridge with battery-backed RAM (8 KB) holds the progress, the partly played levels and
  a long undo history. The 2 KB of built-in RAM is enough for everything else.
- **Speed:** Sokoban is turn-based, so the 1.79 MHz 6502 is plenty.
- **Running it:** a `.nes` file runs on flash cartridges such as the Everdrive N8 Pro (with saves), in Mesen
  or FCEUX, and in the Miyoo's NES emulator.

## Target

| | |
|---|---|
| Mapper | MMC1 (SNROM/SXROM) or MMC3 with 8 KB battery-backed PRG-RAM. MMC3's scanline counter makes a fixed status bar under a scrolling level easy. |
| ROM | 128-256 KB PRG: all 1,093 levels compress small (a few bytes per row), plus the music and code. CHR-RAM or 8-32 KB CHR-ROM for the art. |
| Toolchain | [cc65](https://cc65.github.io/) (C plus ca65 assembly) with a small NES library such as Shiru's neslib, or [llvm-mos](https://llvm-mos.org/). Build a `.nes` from `make nes`. |
| Music | [FamiStudio](https://famistudio.org/) or FamiTone2's engine for the playback; the generative rules (chords, key changes, acorns bringing in the rhythm) as our own code driving it. |
| Testing | Mesen (debugger, Lua scripting) for scripted inputs and screenshots; hardware on a flash cartridge. |

## Screen

The NES screen is 256 x 240; TVs cut off about 8 lines top and bottom, so plan on 256 x 224.

- **16 px squares:** 16 x 14 squares on screen, 16 x 13 with a status row (set, level, moves, pushes).
  About 700 of the 1,093 levels fit on one screen; the rest scroll, following the animal like the zoomed
  view on the Miyoo.
- **Map view (hold):** the whole level at 8 px squares (32 x 28), drawn from small versions of each
  square. That covers every Microban level and most Sasquatch ones; the very largest still scroll.
- **Sprites:** the animal (2 x 2 sprites of 8 x 8, a 16 x 16 animal), the squirrels, fireflies and the
  barn owl. The NES shows at most 8 sprites on one line, so acorns are background tiles, not sprites,
  except the one being pushed (it slides as a sprite, then becomes a tile again).
- **Hedges:** the end caps and joins need their own tiles, as on the Game Boy (the NES background can't
  flip tiles). 256 background tiles is enough for hedges, floor, spots, acorns, the font and the logo.

## Colour

The NES palette has about 54 usable colours, but only 4 background palettes and 4 sprite palettes of 3
colours each at a time.

- **Background:** one palette for hedges, one for the floor and spots, one for acorns, one for text
  and the status bar.
- **Sprites:** one for the animal, one for squirrels, one for fireflies, one for the barn owl.
- **Palettes like Night or Game Boy green** are just different colour choices from the NES palette. Each
  of today's palettes can be matched closely.
- **Fades** (rain dimming, dusk) step through darker versions of each colour.

## Controls

D-pad, A, B, Start and Select, the same as the Game Boy, so the same scheme:

| Today (Miyoo) | NES |
|---|---|
| D-pad move | D-pad move |
| B undo, A redo | B undo, A redo |
| X hold: restart | Select + B (hold): restart, with the same window and bar |
| Y hold: map | Select (hold): map |
| Select: level list | Select (tap): level list |
| L1 / R1 hold: previous / next level | Select + Left / Right (hold) |
| Start: pause menu | Start: pause menu |

## Levels, codes and saves

- **Built-in sets:** all of Microban I-IV and Sasquatch I-XII, credited to David W. Skinner exactly as in
  the README, packed into ROM by a build-time tool with the dead squares precomputed.
- **Level and progress codes:** the same formats as today (see `src/maker.c`), typed on an on-screen
  keyboard, so codes made in the browser or on the Miyoo work here and the other way round.
- **Saves in the 8 KB battery RAM:** solved flags and best moves / pushes for every level (about 5 KB),
  the last few dozen partly played levels with their moves, options, the animal, *Continue*, and levels
  entered from codes.
- **Not coming:** the level maker's solver (it needs megabytes). A level maker on the NES itself is
  possible later (the canvas fits in RAM), but isn't planned.

## Shared core

The NES and the C64 use the same processor and the same compiler (cc65), and the Game Boy's GBDK
compiles similar C. Before any of those ports, pull the parts of the game that don't touch the screen or
sound into one small module of plain, low-memory C:

- moves, pushes, undo and redo
- stuck-acorn checks and the dead squares
- level decoding, level codes and progress codes
- the music's rules (which chord next, when parts come in), separate from the sound chip

Each port then writes only its own graphics, sound and input around it. The Game Boy plan
([docs/GAMEBOY.md](GAMEBOY.md)) would use the same core.

## Steps

1. **Shared core** (above), tested against today's game: same moves, same codes.
2. **Sound test:** a few bars of the generative music on the NES channels.
3. **Prototype:** one level with hedges, the animal, pushing, undo and restart, scrolling for a big level.
4. **Full game:** all level sets, menus, saving, all animals, the squirrels, the map view.
5. **Codes:** the on-screen keyboard, My levels, progress codes.
6. **Listen mode, the barn owl, polish**, then hardware testing on a flash cartridge.

## Open questions

- MMC1 or MMC3 (MMC3 makes the fixed status bar under scrolling simpler).
- How many partly played levels to keep in battery RAM.
- Whether Listen's play-along works well with only A, B and the D-pad.
- PAL timing (50 Hz) for the music, if it matters.
