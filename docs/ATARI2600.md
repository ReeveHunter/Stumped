# Stumped for the Atari 2600 (plan)

**Status:** an idea for later, not started. A small "demake": just the level sets, with simple art,
drums, bass and a few arpeggios. No level maker, no Listen mode, no codes, no long undo history.

## Memory

The 2600 has 128 bytes of RAM in total, for the game's variables, the stack and the board. A level up
to 20 x 16 has 320 squares; one bit each for hedges, spots and acorns is 120 bytes, nearly all of it.
Two ways round that:

- **Keep what never changes in ROM.** Hedges and spots are fixed during a level, so they're stored on
  the cartridge ready to draw (no RAM). Only the acorns (about 40 bytes) and a few variables live in RAM.
- **Or use a cartridge with a little extra RAM** (the classic "Superchip" adds 128 bytes; other schemes
  add 2 KB or more). Modern homebrew cartridges are made with these.

## Screen

- The background is 40 coarse blocks across; each square is 2 blocks wide, so up to 20 squares across
  and about 16 rows on screen.
- Hedges, acorns and spots are block patterns: solid for hedge, a hollow box for an acorn, a dot for a
  spot, a filled box for an acorn on its spot. Colour can change per line, so each row of squares can
  have its own colour band.
- The animal is a real sprite with a little walking animation.
- Drawing an uneven grid every frame (racing the beam) is the trickiest part; it's well-understood
  homebrew technique.

## Levels and play

- The Microban sets mostly fit on one screen; the big levels need scrolling or are left out. All the
  levels compress small enough for a bank-switched cartridge if every level goes in.
- A short undo (the last 10-20 moves costs only a few bytes) and restart.
- No saving needed: every level is open from the start; pick a set and a level and play.

## Sound

Two channels whose pitches don't all line up, so the music is chosen around the notes that do:

- **Drums** from the noise sounds.
- **Bass** on the other channel, in a key chosen for good low notes.
- **When most acorns are on their spots**, the bass channel shares time with fast in-tune arpeggios.
- No key changes like the current music, but a few well-tuned keys per level set keep it varied.

## Building and running

Built and tested in the Stella emulator; runs on real hardware from a flash cartridge such as the
Harmony. A cartridge with an ARM coprocessor (Harmony/Melody with CDFJ, PlusCart, UnoCart) would allow
writing the logic in C, but the basic version doesn't need it.
