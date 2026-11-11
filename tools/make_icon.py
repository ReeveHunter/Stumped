#!/usr/bin/env python3
"""Onion app icon: a tree stump in thin rounded #DDDDDD outlines (matches the "M" theme), 74x74."""
from PIL import Image, ImageDraw
S = 8
W = 74 * S
im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
col = (0xDD, 0xDD, 0xDD, 255)
lw = int(3.2 * S)
thin = int(2.4 * S)
# top of the stump: an ellipse with a growth ring
d.ellipse((18 * S, 16 * S, 56 * S, 32 * S), outline=col, width=lw)
d.ellipse((28 * S, 21 * S, 46 * S, 27 * S), outline=col, width=thin)
# sides
d.line((18.5 * S, 24 * S, 18.5 * S, 50 * S), fill=col, width=lw)
d.line((55.5 * S, 24 * S, 55.5 * S, 50 * S), fill=col, width=lw)
# roots flaring out at the bottom
d.line((18.5 * S, 50 * S, 13 * S, 57 * S), fill=col, width=lw)
d.line((55.5 * S, 50 * S, 61 * S, 57 * S), fill=col, width=lw)
d.line((13 * S, 57 * S, 61 * S, 57 * S), fill=col, width=lw)
im = im.resize((74, 74), Image.LANCZOS)
im.save("onion/icon.png")
