# Stumped for the Wii (plan)

**Status:** an idea for later, not started. Unlike the Game Boy, NES and C64 plans, this is a real port
of `src/main.c`: the same C code, built with the Wii homebrew tools. Everything comes along, including the
level maker, Listen, the music, the codes and the solver.

## Why it's the easiest port

- **Same screen size:** the game draws into its own 640 x 480 picture, and the Wii's normal output is
  640 x 480 (480i or 480p on NTSC). No rescaling or new layouts.
- **Same library:** the Miyoo version uses SDL 1.2, and there are SDL ports for the Wii in the homebrew
  toolchain. Most of the Miyoo-specific code (screen rotation, the Onion launcher, power handling) just
  drops out.
- **Plenty of power:** a 729 MHz PowerPC with 88 MB of RAM. The solver, the music and the drawing all run
  comfortably.
- **Saves:** a plain file on the SD card, the same `save.txt` format as the Miyoo, so a save can be copied
  between the two.

## Target

| | |
|---|---|
| Runs from | The Homebrew Channel. Installed as `apps/stumped/` on the SD card: `boot.dol`, `meta.xml` (name, version, description) and `icon.png` (128 x 48). |
| Toolchain | devkitPro's devkitPPC with libogc, libfat (SD card) and an SDL port from devkitPro's package manager. Build a `boot.dol` from `make PLATFORM=wii`. |
| Saves | `sd:/apps/stumped/save.txt` (and `.bak`), levels from `sd:/apps/stumped/levels/`, so added level sets work like on the Miyoo. |
| Video | 640 x 480, 4:3. On a widescreen TV the Wii stretches it unless set to 4:3; add an option to draw with side bars for 16:9. 50 Hz PAL output needs a 528 or 576-line mode or letterboxing. |
| Sound | SDL audio, the same mixer and music code as today. |
| Testing | The Dolphin emulator (with the Wii Remote emulated) for quick checks; real Wii for controller feel. |

## Controls

Read the controllers directly with libogc (WPAD for Wii Remotes and Classic Controllers, PAD for
GameCube controllers) and turn them into the game's buttons. That's more reliable than going through SDL's
joystick layer.

**Wii Remote held sideways** (the main way to play):

| Game | Wii Remote |
|---|---|
| D-pad | D-pad |
| A (redo, choose) | 2 |
| B (undo, back) | 1 |
| X (hold: restart) | B trigger (hold) |
| Y (hold: map) | A (hold) |
| L1 / R1 (previous / next level) | - / + |
| Start (menu) | Home |
| Select (level list) | *Level list* in the Home menu |

The sideways Remote has no shoulder buttons, so zoom moves to the pause menu (it's already there).

**Classic Controller and GameCube controller:** mapped like a Miyoo, with A, B, X, Y, L, R, ZL/ZR (or Z)
and Start / Select in the same places.

**Pointing (worth trying):** held upright, the Wii Remote is a pointer. The web version's touch mode
already does everything by tapping: tap a square to walk there, BACK, MENU and the page buttons, tap
outside a pop-up to close it. Pointing and pressing A could act as a tap, with a small pointer drawn on
screen. That would make the level maker paint like it does on an iPad.

## Steps

1. **Build:** set up devkitPPC and get today's game to build as a `boot.dol` with the SD card for saves
   and levels.
2. **Controllers:** the sideways Wii Remote, then the Classic and GameCube controllers.
3. **Video:** 480p and 480i, PAL, and the 16:9 option.
4. **Pointer:** the Wii Remote as a pointer using the touch-mode screens.
5. **Homebrew Channel packaging:** `meta.xml`, the icon, a release zip.

## Open questions

- Whether to use SDL 1.2 (closest to the Miyoo build) or SDL2 for the Wii.
- The final Wii Remote button layout, after trying it in hand.
- Whether the pointer replaces the D-pad for menus when the Remote is held upright.
