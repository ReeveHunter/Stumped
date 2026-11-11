#!/usr/bin/env python3
"""
Stumped — Game Boy–style sprites (16×16, 4 shades).

Everything is drawn by hand as letters on a 16×16 grid and written to src/sprites.h as
2-bit shade indices (0 = lightest .. 3 = darkest, 255 = transparent). The game shows these
at 1×, 2×, 3× or 4× with crisp square pixels, and the palette maps the four shades to colours.

    python3 tools/art.py          # writes src/sprites.h and docs/sprites-preview.png

Letters: '.' transparent, 'o' outline (3), 'E' eye (3, two pixels tall so it can blink),
other letters are mapped per animal to shades 0..3.
Frames are derived from the base drawing: blink, happy (^ ^), breathe, hop, lean.
"""
import os, sys

T = 255
FRAMES = ["idle", "blink", "tail", "step", "happy", "push"]

ANIMALS = {
    "Owl": dict(colors=dict(b=2, w=0, i=1, s=2, k=1), rows=[
        "..oo......oo....",
        "..oboooooobo....",
        "..obbbbbbbbbo...",
        "..obwwwbwwwbo...",
        "..obwEwbwEwbo...",
        "..obwEwbwEwbo...",
        "..obwwwowwwbo...",
        "..obbbbobbbbo...",
        ".obbiiiiiiibbo..",
        ".obbisisisibbo..",
        ".oobiiiiiiiboo..",
        ".obbiisisiibbo..",
        "..obiiiiiiibo...",
        "...obbbbbbbo....",
        "....okoooko.....",
        "....ooo.ooo.....",
    ]),
    # sitting turned a little to the right: the near back leg shows on the left, front legs on splayed
    # feet. Breathing puffs the throat out; pushing leans the head and shoulders into it.
    "Frog": dict(colors=dict(b=1, w=0, s=2), rows=[
        "................",
        "................",
        "................",
        "................",
        "....ooo...ooo...",
        "...owwwo.owwwo..",
        "...owEwooowEwo..",
        "..obbEbbbbbEbo..",
        ".obbbbbbbbbbbbo.",
        ".obsbooooooooo..",
        ".obbbbbbwwwwbo..",
        "obbbbbbbwwwwbbo.",
        "obbbbbbobwwbobo.",
        "obbobbbobwwbobo.",
        "oobbobbbowwobbo.",
        ".ooooooooooooo..",
    ], frames=dict(tail=[
        "................",
        "................",
        "................",
        "................",
        "....ooo...ooo...",
        "...owwwo.owwwo..",
        "...owEwooowEwo..",
        "..obbEbbbbbEbo..",
        ".obbbbbbbbbbbbo.",
        ".obsbooooooooo..",
        ".obbbbbbwwwwwo..",
        "obbbbbbwwwwwbbo.",
        "obbbbbbobwwbobo.",
        "obbobbbobwwbobo.",
        "oobbobbbowwobbo.",
        ".ooooooooooooo..",
    ], push=[
        "................",
        "................",
        "................",
        "................",
        ".....ooo...ooo..",
        "....owwwo.owwwo.",
        "....owEwooowEwo.",
        "...obbEbbbbbEbo.",
        "..obbbbbbbbbbbbo",
        "..obsbooooooooo.",
        ".obbbbbbbwwwwbo.",
        "obbbbbbbwwwwbbo.",
        "obbbbbbobwwbobo.",
        "obbobbbobwwbobo.",
        "oobbobbbowwobbo.",
        ".ooooooooooooo..",
    ])),
    # big ears, white cheek ruff, bushy tail standing up behind
    "Fox": dict(colors=dict(b=1, w=0, d=2), rows=[
        "................",
        ".o..........o...",
        ".oo........oo...",
        ".odo......odo...",
        ".oddo....oddo...",
        ".obddooooddbo...",
        ".obbbbbbbbbbo...",
        ".obbEbbbbEbbo.oo",
        "owwbEbbbbEbwwowo",
        ".owwwwwwwwwwoowo",
        "..owwwoowwwoobbo",
        "...obwwwwbo.obbo",
        "..obbwwwwbbobbbo",
        "..obbwwwwbbobbo.",
        "..obobwwbobbbo..",
        "..oooooooooooo..",
    ]),
    # tall ears, a dark mask with light-rimmed eyes, a big ringed tail
    "Raccoon": dict(colors=dict(b=1, w=0, m=2, d=2), rows=[
        "................",
        "..oo......oo....",
        ".obdo....odbo...",
        ".obbo....obbo...",
        ".obbbbbbbbbbo...",
        "obwwbbbbbbwwbo..",
        "ommmmmbbmmmmmo..",
        "omwEmmmmmmEwmo..",
        "ommEmbbbbmEmmooo",
        ".obwwwoowwwbommo",
        "..obwwwwwwbobbbo",
        "..obbwwwwbbommmo",
        "..obbwwwwbbobbbo",
        "..obbwwwwbbommmo",
        "..obobbbbobobbo.",
        "..oooooooooooo..",
    ]),
}
ORDER = ["Owl", "Frog", "Fox", "Raccoon"]


def grid(rows):
    g = [list(r.ljust(16, ".")[:16]) for r in rows]
    assert len(g) == 16, "need 16 rows"
    return g


def eye_cells(g):
    out = []
    for y in range(15):
        for x in range(16):
            if g[y][x] == "E" and g[y + 1][x] == "E" and (y == 0 or g[y - 1][x] != "E"):
                out.append((x, y))
    return out


def frame(name, fr):
    d = ANIMALS[name]
    if fr in d.get("frames", {}):      # drawn by hand for this animal
        cmap = dict(o=3, E=3)
        cmap.update(d["colors"])
        return [[T if ch == "." else cmap.get(ch, 3) for ch in row] for row in grid(d["frames"][fr])]
    g = grid(d["rows"])
    cmap = dict(o=3, E=3)
    cmap.update(d["colors"])
    eyes = eye_cells(g)

    def fill_of(x, y):
        for dx in (-1, 1):
            ch = g[y][x + dx] if 0 <= x + dx < 16 else "."
            if ch not in ("E", "o", "."):
                return ch
        return "b"
    if fr == "blink":
        for x, y in eyes:
            g[y][x] = fill_of(x, y)
    elif fr == "happy":
        for x, y in eyes:
            f = fill_of(x, y)
            g[y + 1][x] = f
            g[y][x] = "o"
            for dx in (-1, 1):
                if 0 <= x + dx < 16 and g[y + 1][x + dx] not in ("o", "."):
                    g[y + 1][x + dx] = "o"
    px = [[T if ch == "." else cmap.get(ch, 3) for ch in row] for row in g]
    if fr == "tail":      # breathe: squash down one pixel above the feet
        px = [[T] * 16] + px[0:13] + px[14:16]
    elif fr == "step":    # hop: stretch up one pixel, feet stay
        px = px[1:13] + [px[12]] + px[13:16]
    elif fr == "push":    # lean forward
        px = [[T] + row[:15] for row in px]
    return px


# ---------------- tiles: a forest clearing at night ----------------

def digits(rows):
    rows = [r.ljust(16, ".")[:16] for r in rows]
    assert len(rows) == 16
    return [[T if ch == "." else int(ch) for ch in r] for r in rows]


def floor(tufts):
    """Plain lightest ground; a hint of grass (tiny 'v' tufts in shade 1)."""
    f = [[0] * 16 for _ in range(16)]
    for (x, y) in tufts:
        f[y][x] = 1; f[y][x + 2] = 1; f[y + 1][x + 1] = 1
    return f


FLOOR_A = floor([])
FLOOR_A[11][4] = 1
FLOOR_B = floor([(3, 4)])
FLOOR_C = floor([(10, 10)])

GOAL = digits([
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
    "0000000220000000",
    "0000002112000000",
    "0000021001200000",
    "0000021001200000",
    "0000002112000000",
    "0000000220000000",
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
    "0000000000000000",
])

ACORN = digits([            # a dark cap with a little shine on it, and a glossy nut
    "................",
    ".......33.......",
    "....33333333....",
    "...3212323233...",
    "..321232323233..",
    ".32222222222223.",
    ".33333333333333.",
    "..310001111123..",
    "..300111111123..",
    "..301111111223..",
    "..311111111223..",
    "...3111111223...",
    "....31111223....",
    ".....332233.....",
    ".......33.......",
    "................",
])

ACORN_GOAL = [row[:] for row in ACORN]
for (x, y) in ((13, 1), (12, 2), (13, 2), (14, 2), (13, 3), (2, 10), (1, 11), (2, 11), (3, 11), (2, 12)):
    ACORN_GOAL[y][x] = 3          # little dark sparkles: this one is home

STUMP = digits([            # the stump: clear rings on top, and roots splaying at the foot
    "................",
    "................",
    "....33333333....",
    "..330000000033..",
    ".30011111111003.",
    ".30100000000103.",
    ".30100111100103.",
    ".30010000001003.",
    ".33001111110033.",
    ".32330000000323.",
    ".32233333333223.",
    ".32232223222323.",
    "322322232223223.",
    "3223222322232223",
    "3232232322232323",
    "33.3333.33333.33",
])



# ---- squirrels (they come out of the hedges to collect the acorns) ----
# drawn from little disks with automatic outlines; the tail and the body are outlined separately
def blank(): return [[T] * 16 for _ in range(16)]
def disk(g,cx,cy,rx,ry,c):
    for y in range(16):
        for x in range(16):
            if ((x+0.5-cx)/rx)**2+((y+0.5-cy)/ry)**2<=1: g[y][x]=c
def outline(g):
    add=[]
    for y in range(16):
        for x in range(16):
            if g[y][x]!=T: continue
            for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)):
                xx,yy=x+dx,y+dy
                if 0<=xx<16 and 0<=yy<16 and g[yy][xx] not in (T,3): add.append((x,y));break
    for x,y in add: g[y][x]=3
def stamp(a,b):
    for y in range(16):
        for x in range(16):
            if b[y][x]!=T: a[y][x]=b[y][x]
def squirrel(frame):
    tail=blank()
    disk(tail,3.8,7.6,3.0,5.0,2)
    disk(tail,5.6,2.6,2.6,1.8,2)
    disk(tail,7.9,3.6,1.1,1.3,2)
    for (x,y) in ((5,2),(4,3),(3,4),(2,5),(2,6),(2,7),(3,8)): tail[y][x]=1   # fluffy highlight curl
    outline(tail)
    body=blank()
    disk(body,9.0,11.0,2.8,2.7,1)
    disk(body,11.8,6.8,2.6,2.3,1)
    for (x,y) in ((11,3),(11,4),(12,4),(14,7),(14,8)): body[y][x]=1
    # legs
    legs={'a':[(6,13),(7,13),(10,13),(11,13)],'b':[(7,13),(8,13),(11,13),(12,13)]}
    for (x,y) in legs['b' if frame=='run2' else 'a']: body[y][x]=1; body[y+1][x]=1
    disk(body,10.6,11.4,1.2,1.6,0)
    outline(body)
    body[6][12]=3; body[7][12]=3      # eye
    body[8][13]=0                      # cheek
    stamp(tail,body)
    if frame=='carry':
        for (x,y,c) in ((13,8,3),(14,8,3),(12,9,3),(13,9,2),(14,9,2),(15,9,3),(12,10,3),(13,10,1),(14,10,1),(15,10,3),(13,11,3),(14,11,3)):
            tail[y][x]=c
    return tail


SQ_RUN1, SQ_RUN2, SQ_CARRY = squirrel("run1"), squirrel("run2"), squirrel("carry")

# ---------------- Listen guests: a barn owl on a stump, and an owlet that follows the owl ----------------

def letters(rows, colors, blink=False):
    g = grid(rows)
    if blink:
        for x, y in eye_cells(g):
            g[y][x] = "w"
    return [[T if ch == "." else 3 if ch in "oE" else colors[ch] for ch in row] for row in g]

GUEST_C = dict(b=1, w=0, i=0, s=2, k=2)
BARN = [
    "................",
    "................",
    "................",
    "....oooooooo....",
    "...obbbbbbbbo...",
    "..obwwwbbwwwbo..",
    "..owwwwwwwwwwo..",
    "..owwEwwwwEwwo..",
    "..owwEwwwwEwwo..",
    "..obwwwkkwwwbo..",
    "..obbwwwwwwbbo..",
    ".obbbbwwwwbbbbo.",
    ".obsbbiiiibbsbo.",
    "..obbsiiiisbbo..",
    "...obbbbbbbbo...",
    "....okoooko.....",
]
BARN_LOOK = [   # head turned to the left (mirrored in the game for the right)
    "................",
    "................",
    "................",
    "....oooooooo....",
    "...obbbbbbbbo...",
    "..obwwbbwwwbbo..",
    "..owwwwwwwwwbo..",
    "..owEwwwwEwwbo..",
    "..owEwwwwEwwbo..",
    "..obwwkkwwwbbo..",
    "..obwwwwwwbbbo..",
    ".obbbbwwwwbbbbo.",
    ".obsbbiiiibbsbo.",
    "..obbsiiiisbbo..",
    "...obbbbbbbbo...",
    "....okoooko.....",
]
# flying: the body with its folded wings opened out (and feet tucked), plus a pair of short, stubby
# wings drawn as their own tiles beside it, so the span can be wider than one square.
BARN_FLY = [
    "................",
    "................",
    "................",
    "....oooooooo....",
    "...obbbbbbbbo...",
    "..obwwwbbwwwbo..",
    "..owwwwwwwwwwo..",
    "..owwEwwwwEwwo..",
    "..owwEwwwwEwwo..",
    "..obwwwkkwwwbo..",
    "..obbwwwwwwbbo..",
    "..obbbwwwwbbbo..",
    "..obbiiiiiibbo..",
    "...obiiiiiibo...",
    "....obbbbbbo....",
    "................",
]

def barn_wing(angle, length=8.5, root_w=6.0, tip_w=4.0):
    """the left wing in a 16x16 tile: its root sits just past the right edge, at the owl's shoulder
       (the game draws it 14 pixels left of the body, and mirrored 13 pixels right for the other wing).
       angle: raised this many degrees (negative = down)."""
    import math
    a = math.radians(angle)
    ux, uy = -math.cos(a), -math.sin(a)          # out along the wing
    vx, vy = -uy, ux                              # across it, towards the leading edge
    if vy > 0:
        vx, vy = -vx, -vy
    pts = {}
    for y in range(16):
        for x in range(16):
            dx, dy = x + .5 - 16.5, y + .5 - 10.5
            s_ = dx * ux + dy * uy
            t_ = dx * vx + dy * vy
            if s_ < 0 or s_ > length:
                continue
            f = s_ / length
            w = root_w * (1 - f) + tip_w * f
            if f > 0.85:
                w *= math.sqrt(max(0, 1 - ((f - 0.85) / 0.15) ** 2)) * 0.9 + 0.1
            lo, hi = -w * 0.35, w * 0.65
            if lo <= t_ <= hi:
                pts[(x, y)] = (s_, t_, lo, hi)
    g = [[T] * 16 for _ in range(16)]
    for (x, y), (s_, t_, lo, hi) in pts.items():
        edge = any((x + i, y + j) not in pts for i, j in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        if edge:
            v = 3
        elif t_ > hi - 1.6:
            v = 0                                  # pale leading edge
        elif t_ < lo + 1.8 and s_ > length * 0.4:
            v = 2                                  # darker flight feathers
        else:
            v = 1
        g[y][x] = v
    return g

OWLET = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "....oooooooo....",
    "...obbbbbbbbo...",
    "..obwwwbbwwwbo..",
    "..obwEwbbwEwbo..",
    "..obwEwkkwEwbo..",
    "..obbwwbbwwbbo..",
    ".obbiiiiiiiibbo.",
    ".obbiisiisiibbo.",
    "..obbiiiiiibbo..",
    "...okkooookko...",
]

TILES = [("floor", FLOOR_A), ("floor_b", FLOOR_B), ("floor_c", FLOOR_C), ("goal", GOAL),
         ("acorn", ACORN), ("acorn_goal", ACORN_GOAL), ("stump", STUMP),
         ("squirrel", SQ_RUN1), ("squirrel_b", SQ_RUN2), ("squirrel_carry", SQ_CARRY),
         ("barn", letters(BARN, GUEST_C)), ("barn_look", letters(BARN_LOOK, GUEST_C)),
         ("barn_blink", letters(BARN, GUEST_C, True)),
         ("owlet", letters(OWLET, GUEST_C)), ("owlet_blink", letters(OWLET, GUEST_C, True)),
         ("barn_fly", letters(BARN_FLY, GUEST_C)),
         ("barn_wing_up", barn_wing(55)), ("barn_wing_mid", barn_wing(10)), ("barn_wing_down", barn_wing(-35))]


# ---------------- hedges: 8x8 quadrants that join up ----------------
# Each hedge cell is built from four quadrants (TL, TR, BL, BR). A quadrant looks at its two
# outer neighbours and the diagonal between them and picks a type:
#   OUTER  neither neighbour is hedge          -> a rounded corner
#   EDGE_V only the side neighbour is hedge    -> open above/below: a leafy top or bottom edge
#   EDGE_H only the vertical neighbour         -> open to the side: a leafy side edge
#   INNER  both neighbours, not the diagonal   -> a small notch in the corner
#   FULL   all three                           -> solid leaves (plus berry / flower variants)
# Where two hedges only meet corner to corner, the empty square between them (outside the level)
# gets their end caps instead of being filled in:
#   CAP_V  the hedge beside it ends here: a leafy top, rounded off toward the middle of the square
#   CAP_H  the same for the hedge above or below: a leafy side, rounded off toward the middle

HEDGE_TEX = digits([
    "2212222222122222",
    "2111232221112322",
    "2223222222232222",
    "2222222222222222",
    "2222221222222212",
    "3222211122322111",
    "2322222232222222",
    "2222222222222222",
    "2222122222221222",
    "2232111222321112",
    "2222232222222322",
    "2222222222222222",
    "1222222212222222",
    "1122322111223222",
    "2222232222222322",
    "2222222222222222",
])
QT = ["full", "full_berry", "full_flower", "edge_v", "edge_h", "outer", "inner", "cap_v", "cap_h"]
SCALLOP = [2, 1, 1, 2, 2, 1, 1, 2]


def quad_inside(qt):
    """8x8 bool mask in top-left orientation (x=0 outer left, y=0 outer top)."""
    m = [[True] * 8 for _ in range(8)]
    for y in range(8):
        for x in range(8):
            if qt == "edge_v":
                m[y][x] = y >= SCALLOP[x]
            elif qt == "edge_h":
                m[y][x] = x >= SCALLOP[y]
            elif qt == "outer":
                m[y][x] = y >= SCALLOP[x] and x >= SCALLOP[y] and (7.6 - x) ** 2 + (7.6 - y) ** 2 <= 6.9 ** 2
            elif qt == "inner":
                m[y][x] = not (x + y <= 1)
            elif qt == "cap_v":     # continues at x = 0, open at the top, rounded off toward x = 7
                m[y][x] = y >= SCALLOP[x] and (x + 0.6) ** 2 + (7.6 - y) ** 2 <= 6.9 ** 2
            elif qt == "cap_h":     # continues at y = 0, open at the left, rounded off toward y = 7
                m[y][x] = x >= SCALLOP[y] and (y + 0.6) ** 2 + (7.6 - x) ** 2 <= 6.9 ** 2
    return m


def quadrant(qt, q):
    """q: 0 TL, 1 TR, 2 BL, 3 BR. Returns 8x8 pixels sampled from the hedge texture."""
    m = quad_inside(qt)
    # outside the quadrant: beyond the outer edges is open unless that neighbour is hedge
    open_top = qt in ("edge_v", "outer", "cap_v")
    open_left = qt in ("edge_h", "outer", "cap_h")

    def inside(x, y):
        if x < 0: return not open_left
        if y < 0: return not open_top
        if x > 7 or y > 7: return True
        return m[y][x]
    px = [[T] * 8 for _ in range(8)]
    fx, fy = q % 2 == 1, q >= 2
    ox, oy = (8 if fx else 0), (8 if fy else 0)
    for y in range(8):
        for x in range(8):
            if not m[y][x]:
                continue
            edge = not all(inside(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            # where this pixel lands in the 16x16 tile (after mirroring)
            tx = (7 - x if fx else x) + ox
            ty = (7 - y if fy else y) + oy
            v = 3 if edge else HEDGE_TEX[ty][tx]
            # moonlight on top edges
            if not edge and not fy and open_top and y > 0 and not inside(x, y - 2):
                v = 1
            px[ty - oy][tx - ox] = v
    if qt == "full_berry":
        for (x, y) in ((2, 3), (4, 5)):
            px[y][x] = 3; px[y][x + 1] = 3; px[y + 1][x] = 3; px[y + 1][x + 1] = 3; px[y][x] = 0
    if qt == "full_flower":
        cx, cy = 4, 3
        for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
            px[cy + dy][cx + dx] = 0
        px[cy][cx] = 3
    return px


HEDGEQ = [[quadrant(qt, q) for q in range(4)] for qt in QT]


# ---------------- output ----------------

def fmt(data):
    return "\n".join(",".join(str(v) for v in data[i:i + 32]) + "," for i in range(0, len(data), 32))


def emit(path):
    out = ["/* Generated by tools/art.py — do not edit by hand. */", "#pragma once", "#include <stdint.h>",
           f"#define NUM_ANIMALS {len(ORDER)}", f"#define NUM_FRAMES {len(FRAMES)}", f"#define NUM_TILES {len(TILES)}",
           "#define SPR 16", "#define SPR_T 255",
           "static const char *const ANIMAL_NAMES[NUM_ANIMALS] = {" + ", ".join(f'"{a}"' for a in ORDER) + "};"]
    out += [f"#define FR_{f.upper()} {i}" for i, f in enumerate(FRAMES)]
    out += [f"#define TL_{t.upper()} {i}" for i, (t, _) in enumerate(TILES)]
    data = [v for a in ORDER for fr in FRAMES for row in frame(a, fr) for v in row]
    out += ["static const uint8_t ANIM[NUM_ANIMALS][NUM_FRAMES][16][16] = {", fmt(data), "};"]
    data = [v for _, t in TILES for row in t for v in row]
    out += ["static const uint8_t TILE[NUM_TILES][16][16] = {", fmt(data), "};"]
    out += [f"#define NUM_QT {len(QT)}"] + [f"#define QT_{q.upper()} {i}" for i, q in enumerate(QT)]
    data = [v for qt in HEDGEQ for qd in qt for row in qd for v in row]
    out += ["static const uint8_t HEDGEQ[NUM_QT][4][8][8] = {", fmt(data), "};"]
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


PREVIEW_PAL = [(0xE0, 0xF8, 0xD0), (0x88, 0xC0, 0x70), (0x34, 0x68, 0x56), (0x08, 0x18, 0x20)]


def preview(path, scale=6):
    """Animals (all frames), then the tiles, then every hedge quadrant type."""
    from PIL import Image
    cell = 18
    cols = len(FRAMES)
    trows = (len(TILES) + cols - 1) // cols
    img = Image.new("RGB", (max(8, len(QT)) * cell + 2, (len(ORDER) + trows + 1) * cell + 2), PREVIEW_PAL[0])
    for ai, a in enumerate(ORDER):
        for fi, fr in enumerate(FRAMES):
            blit(img, frame(a, fr), 2 + fi * cell, 2 + ai * cell)
    for ti, (_, t) in enumerate(TILES):
        blit(img, t, 2 + (ti % cols) * cell, 2 + (len(ORDER) + ti // cols) * cell)
    y = 2 + (len(ORDER) + trows) * cell
    for qi in range(len(QT)):
        for q in range(4):
            qp = HEDGEQ[qi][q]
            for yy in range(8):
                for xx in range(8):
                    v = qp[yy][xx]
                    if v != T:
                        img.putpixel((2 + qi * 18 + (q % 2) * 8 + xx, y + (q // 2) * 8 + yy), PREVIEW_PAL[v])
    img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(path)


def blit(img, px, x0, y0):
    for y in range(16):
        for x in range(16):
            v = px[y][x]
            if v != T:
                img.putpixel((x0 + x, y0 + y), PREVIEW_PAL[v])


if __name__ == "__main__":
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    emit(os.path.join(root, "src", "sprites.h"))
    preview(sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "docs", "sprites-preview.png"))
    print("ok")
