# Stumped for Game Boy (plan)

**Status:** planned, not started. The level maker and its codes are done (see `src/maker.c`), and the
Game Boy version will read the same codes (see [Levels](#levels)).

The goal is one cartridge that plays on an original Game Boy (DMG) in 4 shades and in full color on a
Game Boy Color, with a custom border on the Super Game Boy. It is a rewrite, not a port of `src/main.c`:
the puzzles, art and music ideas carry over, the code does not.

## Target

| | |
|---|---|
| Cartridge | Dual mode ("black cartridge"): header byte `0x143 = 0x80`. Runs on DMG, Pocket, GBC, GBA, SGB. |
| Mapper | MBC5 with battery-backed SRAM (32 KB) for saves. ROM size as needed (levels compress well). |
| Toolchain | [GBDK-2020](https://github.com/gbdk-2020/gbdk-2020) (C). Build a `.gbc` from `make gb`. |
| Testing | Headless emulator (e.g. PyBoy or SameBoy) with scripted inputs and screenshots, in both DMG and GBC mode. Hardware testing on a flash cart. |
| On the Miyoo | Onion's Game Boy emulator runs it directly; GBC mode by default, DMG mode can be forced. |

### Rule of thumb

Design for the **DMG first**, then layer color on top. Nothing may depend on Color-only features:

- Art must read in 4 grays (sprites: 3 grays + transparent).
- No per-tile flipping or second VRAM bank for the background on DMG, so hedge corners need their own tiles.
- Must run at normal speed with 8 KB of work RAM. GBC double speed and extra RAM are optional extras.
- On boot, check the A register (`0x11` = Color) and load gray or color palettes accordingly.

## Screen and tile size (open: decide later)

The Game Boy screen is 160×144 (20×18 tiles of 8×8). Two options, keep both open:

| | 8 px squares | 16 px squares |
|---|---|---|
| Level area on one screen | 20×16 (two rows for status) | 10×8 (one row for status) |
| Built-in levels that fit without scrolling | 838 of 1093 | 182 to 268 (depending on status bar) |
| Animals | 1 tile each; would need to be redrawn as tiny characters | 2×2 tiles; close to today's sprites |
| Hedges, acorns, stumps | Simple shapes; less character | Current look survives |
| Map / zoom | Scroll for big levels | Need a separate map view (e.g. switch to 8 px) |

**Corners where hedges only meet corner to corner** get end caps (see `fill_diagonal_gaps` in
`src/main.c`): the two hedges reach into the empty square with rounded tips that overlap. At 16 px this
is just two more quarter-tile types (cap_v, cap_h in `tools/art.py`); the DMG can't flip background
tiles, so every quarter type is stored in all four orientations (about 9 x 4 = 36 hedge tiles, well
within the 256 available). At 8 px each square is one tile chosen from its eight neighbours (about 47
tiles for a full set) plus a handful of caps, so that size needs a new, smaller hedge set.

A middle option is to play at 16 px with the camera following the animal, and let **hold Select** show the
level at 8 px as the map (like *Hold Y* today). The largest built-in level is 49×44, which scrolls either way.

## Controls (open)

The Game Boy has only D-pad, A, B, Start and Select: no X, Y, L or R. A first draft:

| Today (Miyoo) | Game Boy |
|---|---|
| D-pad move | D-pad move |
| B undo, A redo | B undo, A redo |
| X hold: restart | Select + B (hold): restart, with the same window and bar |
| Y hold: map | Select (hold): map |
| Select: level list | Select (tap): level list |
| L1 / R1 hold: previous / next level | Select + Left / Right (hold), same skip window and quick-skip after |
| Start: pause menu | Start: pause menu (Resume, Restart level, Level list, Best run, Favorite, Zoom, Options, Title screen) |

## Levels

- **Built-in sets:** all of Microban I–IV and Sasquatch I–XII, credited to David W. Skinner exactly as in
  the README. A build-time tool (Python, like `tools/art.py`) packs the `levels/*.txt` files into ROM with
  simple run-length compression and precomputes the dead squares.
- **Shared format with the level maker (done):** level codes are capped at 20 x 16 (one screen at 8 px
  squares), use Crockford base-32 letters and digits (no I, L, O, U), and end in a 10-bit checksum. The
  format is a bit stream: version, width, height, then the squares with short prefix codes and run lengths
  for floor and hedges (see `code_from_map` in `src/maker.c`). A typical Microban-sized level is about
  20-35 characters. Decoding is a few dozen lines of C and fits the Game Boy easily.
- **The solver** in the level maker won't come to the Game Boy (it needs megabytes of memory); there the
  clear check (you solve it yourself) is what proves a level works, as it does everywhere.
- **Level codes on the Game Boy:** an on-screen keyboard for entering a code; custom levels are stored in
  SRAM in a "My levels" set. (Building levels *on* the Game Boy is not planned.)

## Saves (SRAM)

- Solved flag and best moves / pushes for every level (about 5 bytes × 1093 levels).
- Partly played levels: today every started level is saved. On the Game Boy, keep the last N started
  levels (N chosen to fit; a few dozen is easy) with their undo history trimmed if needed.
- Options, chosen animal, last level for *Continue*, and stored custom levels.

## Features to carry over

**Core game**

- Pushing acorns onto spots, undo/redo, restart with the moving bar and pop-up, held level switching with
  the quick-skip window, *Continue / Start over* for partly played levels.
- Dead-square warning (precomputed at build time).
- Level list with sets, solved marks and started bars; title screen with the vine logo; Options (with
  Credits inside).
- The four animals (frog, fox, raccoon, owl, plus Random) with idle animations and a happy dance; squirrels
  collecting acorns at the end. Animal choice in Options.

**Barn owl and guests**

- The barn owl swooping in onto a stump on about one level in four, flapping as it lands and takes off,
  watching the player left / right / forward while perched, and always drawn above characters.
- Sprite budget: max 10 sprites per scanline. Owl with wings ~4 sprites wide, animal 2: fine, but keep
  squirrels and the owl from lining up on the same rows if possible.

**Listen mode** (no sleep timer)

- A generated clearing, the animals wandering and gathering, the owlet following the Owl, the squirrel,
  the barn owl on a stump, rain when it's held.
- The same two play-along modes, switched with Select: *Melody* (the D-pad's eight directions are eight
  scale notes from the chord's root; the lead goes on a pulse channel) and *Drums* (eight hits on the noise
  and wave channels). The Game Boy has no shoulder buttons, so Melody's A and B could be octave and
  vibrato, and the held effects (chop, warble, rain, double) would move to Start + a direction, with the
  menu on a tap of Start.

**Super Game Boy**

- A custom border (forest / vine theme) and a palette for the play screen. Optional, added near the end.

**Not carried over**

- Touch, keyboard and web controls; the web build stays as it is.
- The sleep timer.
- Zoom levels beyond the two tile sizes.

## Color

- GBC: 8 background palettes and 8 sprite palettes. Hedges, floor, acorns, stumps and each animal get
  their own colors; the barn owl gets its own sprite palette.
- Fades: 4-step palette fades on DMG; smooth fades and tints (dusk, rain dimming) on GBC.
- Rain on DMG is falling drops only; on GBC it also dims the palette.

## Music and sound

Same sound hardware on DMG and GBC: two pulse channels, one wave channel, one noise channel.

| Channel | Role |
|---|---|
| Wave | **Chord pad.** One 32-step waveform holds a whole chord: a major triad is overtones 4, 5 and 6 of a note two octaves below the root (minor, sus and fifths work the same way). Several brightness versions of each chord give a slow "filter opening" effect. |
| Pulse 1 | Melody, plinks and runs (duty changes for color). |
| Pulse 2 | Bass, or a quiet delayed copy of the melody to fake the reverb; slightly detuned against the pad for chorus when free. |
| Noise | Drums and rain. |

- The generative rules (chords, key changes on clearing a level, acorns bringing in the rhythm) port as
  logic; the sound is a chiptune version of today's music.
- Catches: the wave channel has 4 volume steps only, so fades mean rewriting the waveform at lower levels;
  change waveforms at chord changes or quiet moments to avoid clicks; 4-bit samples add a little grit.
- Warble: pitch wobble on the wave channel. Gate: stepping the wave channel volume.

## Steps

0. **Shared core** (see [docs/NES.md](NES.md#shared-core)): moves, undo, stuck checks, levels and codes in
   plain C that the Game Boy, NES and C64 versions all use.
1. **Sound test:** a few bars of the generative music on Game Boy channels, to judge the chord-wave pad.
2. **Prototype:** one level with hedges, one animal, pushing, undo and restart, in both DMG and GBC mode,
   at both tile sizes so the size question can be decided.
3. **Full game:** all level sets, menus, saving, all animals, the squirrels.
4. **Level codes:** code entry and "My levels", reading the level maker's codes.
5. **Music engine** in the game, then **Listen mode**.
6. **Barn owl, Super Game Boy border, polish**, hardware testing on a flash cart.

## Open questions

- 8 px or 16 px squares (or 16 px with an 8 px map).
- Final button mapping for restart, map, level switching and the Listen play-along.
- How many partly played levels to keep in SRAM.
