"""web/icon.png: the stump tile on a patch of ground in the Night palette, for browser tabs and home screens.
Run it again whenever the stump art changes (python3 tools/art.py first)."""
import re
from PIL import Image

src = open("src/sprites.h").read()
body = src[src.index("TILE[NUM_TILES][16][16]"):]
stump_idx = int(re.search(r"#define TL_STUMP (\d+)", src).group(1))
nums = [int(n) for n in re.findall(r"\d+", body[body.index("{"):])][: (stump_idx + 1) * 256]
px = nums[stump_idx * 256:]
ground = nums[:256]                   # TILE 0 is the floor
NIGHT = [(0x9A, 0xA8, 0xD8), (0x5C, 0x6C, 0xA8), (0x2E, 0x38, 0x68), (0x0C, 0x10, 0x28)]
S, PAD = 10, 10                       # 16 px art at 10x with a 1-pixel border: 180 x 180
img = Image.new("RGB", (16 * S + 2 * PAD, 16 * S + 2 * PAD), NIGHT[ground[0] & 3])
# centre the stump: skip its empty top rows, so it sits in the middle of the square
top = next(y for y in range(16) if any(px[y * 16 + x] != 255 for x in range(16)))
shift = top // 2
for y in range(16):
    for x in range(16):
        sy = y + shift
        v = px[sy * 16 + x] if sy < 16 else 255
        if v == 255: v = ground[y * 16 + x]
        if v != 255:
            img.paste(NIGHT[v & 3], (PAD + x * S, PAD + y * S, PAD + (x + 1) * S, PAD + (y + 1) * S))
img.save("web/icon.png")
print("web/icon.png", img.size)
