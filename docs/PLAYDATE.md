# Stumped for Playdate (plan)

**Status:** planned for after v1.0, using the Playdate SDK's C API and its Simulator. Like the Wii version and unlike the Game Boy, NES, C64 and Atari ones, this is a **port, not a rewrite**: the Playdate
runs C on a 168 MHz ARM with 16 MB of RAM, so nearly all of `src/main.c`, `src/audio.c` and
`src/maker.c` (puzzles, menus, level maker, solver, codes, Listen) carry over. The work is a new
platform layer, a one-bit art pass and controls built around the crank.

## The screen: 400 x 240, black and white

- **The shortcut:** today's screens are 640 x 480 drawn at 2x (text, sprites and menus all use
  2-pixel steps), so they're really 320 x 240 screens shown double. Drawing as today, then taking one
  pixel from each 2 x 2 block gives a crisp 320 x 240 picture with nothing lost. Centred on the
  Playdate's 400 x 240, that leaves a 40-pixel band each side (a good place for the move and push
  counts in play). That's the first prototype; screens can then be laid out for the full width one
  at a time.
- **Squares:** the art is 16 x 16, so levels play at 16-pixel squares (25 x 15 on screen, so most
  levels fit whole) and zoom to 32 for small ones. The Playdate's sharp screen makes 16-pixel art look
  good.
- **Four shades to one bit:** black, white and two dither patterns (a light and a dark one) for the
  in-between shades, done as the frame goes to the screen. Some sprites will want touching up for one
  bit (clear black outlines, white fills, one dither for shading): the acorns, the animals' faces, the
  hedge leaves. Worth a mock-up first, like the C64.
- **Palettes:** no colour, so the nine palettes become a light and a dark look (dark like Night),
  plus maybe a choice of dither patterns. The system's "invert" setting also works.
- **The corner end caps, stuck-acorn shakes, squirrels, barn owl and fireflies** all come along as they are.

## Controls: D-pad, A, B, the crank, and the Menu button

There's no X, Y, L, R, Select or Start, so:

| Today | Playdate (first draft) |
|---|---|
| D-pad move | D-pad move |
| B undo (hold to rewind), A redo | B undo (hold to rewind), A redo |
| | **Crank: scrub through your moves**, backwards to undo, forwards to redo, a move per notch |
| X hold: restart | System menu (Menu button) item: Restart |
| Y hold: map | Crank docked/undocked, or hold A + B: the whole level in view |
| Select: level list | System menu item: Level list |
| L1 / R1: previous / next level | In the level list, or hold A or B + Left / Right |
| Start: pause menu | System menu item: Options (the rest of today's pause menu) |

The system menu holds up to three items of our own. The Clear window, level list and maker need only
the D-pad, A and B.

- **Level maker:** D-pad to move, A to place or remove, B to erase; the crank turns through the tools.
  Undo / redo on the crank, like in play.
- **Codes:** our own on-screen keyboard, as on the Miyoo (the Playdate's built-in keyboard is for Lua
  games). QR codes show beautifully on a one-bit screen.

## Sound

- **Simplest:** the Playdate lets a C game feed its own sound in, so `src/audio.c` can run as is, as a
  custom sound source at 44.1 kHz. Needs measuring on the device: if the pads and rain cost too much,
  turn down their detail on the Playdate rather than rewriting.
- **Listen mode:** the D-pad's eight directions still play the scale; A and B take the octave glide and
  vibrato; **the crank bends the chord or sweeps the filter**, a natural fit. Voices and the held
  effects go in Listen's menu.
- The sleep timer works as today; the system's auto-lock is held off while Listen plays.

## Saves, solver, extras

- **Saves:** `save.txt` (same format as the Miyoo and web) in the game's Data folder, with the backup.
  Progress codes and level codes work the same as everywhere.
- **Solver:** "Solve it for me" runs, with a smaller state limit than the Miyoo to fit in 16 MB.
- **Distribution:** a `.pdx`, sideloaded through a play.date account or shared on itch.io. Selling it in
  Catalog would need the Microban / Sasquatch licence question settled first (they're free to share
  with credit; selling is a different question).

## Steps

1. **Platform layer** (`src/playdate.c`): the update loop, buttons and crank into the game's keys, the
   2 x 2 shrink and dither to the screen, sound as a custom source, files in the Data folder.
2. **Prototype in the Simulator:** title screen, one level, undo on the crank.
3. **One-bit art pass:** dither choices, sprite touch-ups, the light and dark looks.
4. **Controls:** the system menu items, the map, level switching, the maker and Listen on the crank.
5. **Use the full 400 width** in play and the level list.
6. **On a Playdate:** speed, sound and battery; tune the solver limit and the sound detail.
7. **Release.**

## Open questions

- Crank: one move per how many degrees, and does scrubbing play the moves (animated) or jump?
- Map: crank docking, hold A + B, or both?
- 16-pixel squares by default, or 32 with the camera following, like the Game Boy question?
- How much of the art to redraw for one bit versus dithering it as is.
