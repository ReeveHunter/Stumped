# Building Stumped

## SD card layout

```
/mnt/SDCARD/App/Stumped/                ← copy dist/Stumped here
├── stumped                             ← the game
├── launch.sh, config.json, icon.png
├── levels/                             ← one .txt per set (sorted by file name)
├── CREDITS.txt
├── save.txt                            ← created on first run: settings, progress, bests
├── save.txt.bak                        ← the save before, in case the latest is ever damaged
└── log.txt
```

## Version numbers

The version number lives in `VERSION` (shown on the Credits screen, which is in Options, and on the web
page). Each build that goes out adds 0.01 (`python3 tools/bump_version.py`), counting up to 1.0.

### For the Miyoo Mini

A prebuilt `dist/Stumped/` is included, so you can copy it to `/mnt/SDCARD/App/`. To rebuild, use the same
toolchain as other Onion apps ([shauninman/union-miyoomini-toolchain](https://github.com/shauninman/union-miyoomini-toolchain)):

```sh
# inside the toolchain container (it sets CROSS_COMPILE and PREFIX)
cd /root/workspace/Stumped
make PLATFORM=miyoo package
```

The launcher uses Onion's own SDL library and routes audio through Onion's audioserver (`libpadsp`),
as other Onion apps do. If the game won't start, check `App/Stumped/log.txt`.

### For the web

The browser version is the same source compiled with [Emscripten](https://emscripten.org). The code
that differs is in `#ifdef __EMSCRIPTEN__` blocks in `src/main.c` and `src/audio.c`: the game draws into its
own framebuffer, which the device copies to the screen with SDL and the browser copies to a `<canvas>`.
Sound comes from the same mixer through Web Audio; input arrives from the page's keyboard and touch handlers.
The page itself (layout, touch buttons, key map, saving) is `web/index.html`. Any change to the game shows up
in both versions the next time each one is built.

```sh
brew install emscripten            # once
make web WEB_OUT=some/folder       # writes the site into that folder
```

Without `WEB_OUT` it writes into the `stumped/` folder of the website's repo, next to this one.
The output is `index.html`, `stumped.js`, `stumped.wasm` (the levels are packed inside), `icon.png`
and `CREDITS.txt`. Each build stamps its files so browsers don't mix an old `stumped.js` with a new
`stumped.wasm`. `python3 tools/make_web_icon.py` redraws `web/icon.png`.

### On a desktop (for testing)

```sh
brew install sdl12-compat          # or: sudo apt install libsdl1.2-dev
make
./stumped
```

Keyboard: arrows, Space = A, Left Ctrl = B, Left Shift = X, Left Alt = Y, E/T = L1/R1,
Tab/Backspace = L2/R2, Right Ctrl = Select, Enter = Start.

Testing flags:
- `--level SET:N` jumps straight into a level (e.g. `--level 5:12` is Sasquatch 12)
- `--animal N` (0–3, −1 = random) and `--palette N` (0–8)
- `--keys "RIGHT RIGHT PRESS:X WAIT900 RELEASE:X"` sends scripted presses (`--delay ms` between them)
- `--shot out.bmp` saves a screenshot when the script ends
- `--wav out.wav SECONDS` renders the music offline with a short scripted bit of play (acorns landing,
  undo, a level clear, menus) and prints the events and chord/key changes
- `--rotate` / `--no-rotate` override the 180° screen flip (on by default on the device)

### Fewest moves and pushes (offline solver)

`tools/fewest/` is a standalone solver that proves the fewest possible moves and the fewest possible
pushes for each level. It keeps the results in `data/fewest.txt` (moves) and `data/fewest-pushes.txt`
(pushes), with the full solution for each. It skips levels already in the files, so it can be stopped
with Ctrl-C and run again later. On a Mac it needs only the command-line tools (`xcode-select --install`):

```sh
make fewest
./fewest                  # the four Microban sets; -m MB per level (default 3000), -t seconds (default 1800)
```

Levels it can't prove within the limits go in `data/fewest-failed.txt` with the smallest count still
possible, and are retried only with more memory or time (or `-r`, or after the solver improves).
For a long unattended run on a Mac with 16 GB of memory: `caffeinate -ims ./fewest -m 13000 -t 7200`.

Solutions found online can help: save pages with tables of solutions (like the Soko++ solutions list)
in `data/online/` and run `python3 tools/check_online.py`. It replays them, writes the valid ones to
`data/online-best.txt`, warns if one beats a count we call proven, and lists the range each unproven
count must lie in. Those two are other people's solutions, so they stay on your own computer and aren't
part of the repository (see `.gitignore`). The solver then only looks for solutions better than the best known one, and if
there are none, the known one is proven to be the fewest.

`python3 tools/make_fewest.py` (run by `make` when the data changes) replays every solution and writes
`src/fewest.h`. The game shows the proven counts in the level list ("Fewest 23 / 7", `?` where not
proven) and on the Clear window, with a star when your best matches one.

## Art

`tools/art.py` holds every sprite as letters on a 16×16 grid and generates `src/sprites.h`. Each
animal is one drawing, and its other frames are derived from it unless the animal draws its own. Hedges
are 8×8 quarter-tiles that the game picks per corner, so hedges of any shape join up.

Every sprite uses 4 shades (0 = lightest … 3 = darkest), which the palette maps to colours at run time.

```sh
python3 tools/art.py                 # regenerate src/sprites.h and docs/sprites-preview.png
python3 tools/logo.py                # src/logo.h, the title logo (and docs/logo-preview.png)
python3 tools/make_icon.py           # onion/icon.png (M-theme style: a stump in thin #DDDDDD outlines)
python3 tools/import_levels.py SRC levels   # re-import the level sets
```
