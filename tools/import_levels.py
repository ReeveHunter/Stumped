#!/usr/bin/env python3
"""Import David W. Skinner's Sokoban sets (.sok/.txt) into Stumped's compact level files.
Levels by David W. Skinner — "These sets may be freely distributed provided they remain properly credited."
Output: levels/<NN>-<Set Name>.txt with a credit header and one level per block separated by '; N' lines."""
import os, sys, re
SRC = sys.argv[1]; OUT = sys.argv[2]
SETS = [
 ("Microban",       "DavidWSkinner_Microban.sok"),
 ("Microban II",    "DavidWSkinner_Microban_II.sok"),
 ("Microban III",   "DavidWSkinner_Microban_III.sok"),
 ("Microban IV",    "David_W_Skinner_-_Microban_IV.sok"),
 ("Sasquatch",      "DavidWSkinner_Sasquatch_I.sok"),
 ("Sasquatch II",   "DavidWSkinner_Sasquatch_II.sok"),
 ("Sasquatch III",  "DavidWSkinner_Sasquatch_III.sok"),
 ("Sasquatch IV",   "DavidWSkinner_Sasquatch_IV.sok"),
 ("Sasquatch V",    "DavidWSkinner_Sasquatch_V.sok"),
 ("Sasquatch VI",   "DavidWSkinner_Sasquatch_VI.sok"),
 ("Sasquatch VII",  "DavidWSkinner_Sasquatch_VII.sok"),
 ("Sasquatch VIII", "Sasquatch_VIII.sok"),
 ("Sasquatch IX",   "Sasquatch_IX.sok"),
 ("Sasquatch X",    "DavidWSkinner_Sasquatch_X.sok"),
 ("Sasquatch XI",   "DavidWSkinner_Sasquatch_XI.sok"),
 ("Sasquatch XII",  "David_W_Skinner_-_Sasquatch_XII.sok"),
]
MAPCH = set(" #@$.*+-_")
def is_map(line):
    s = line.rstrip()
    return s and set(s) <= MAPCH and '#' in s

def parse(path):
    t = open(path, encoding='latin-1').read().replace('\r', '')
    lv, cur = [], []
    for line in t.split('\n'):
        if is_map(line): cur.append(line.rstrip().replace('-', ' ').replace('_', ' '))
        else:
            if cur: lv.append(cur); cur = []
    if cur: lv.append(cur)
    return lv

def check(m):
    boxes = sum(r.count('$') + r.count('*') for r in m)
    goals = sum(r.count('.') + r.count('*') + r.count('+') for r in m)
    pl = sum(r.count('@') + r.count('+') for r in m)
    return boxes == goals and pl == 1 and boxes > 0, boxes

os.makedirs(OUT, exist_ok=True)
total = 0; maxw = maxh = 0; big = 0
for i, (name, fn) in enumerate(SETS):
    lv = parse(os.path.join(SRC, fn))
    bad = [k + 1 for k, m in enumerate(lv) if not check(m)[0]]
    w = max(max(len(r) for r in m) for m in lv); h = max(len(m) for m in lv)
    nbig = sum(1 for m in lv if max(len(r) for r in m) > 40 or len(m) > 27)
    big += nbig; maxw = max(maxw, w); maxh = max(maxh, h); total += len(lv)
    print(f"{name:15s} {len(lv):4d} levels  max {w}x{h}  needs-scroll@16px: {nbig}  bad: {bad}")
    with open(os.path.join(OUT, f"{i+1:02d}-{name}.txt"), 'w') as f:
        f.write(f"; {name}\n; Copyright: David W. Skinner, Grandview, Washington, USA\n"
                f"; \"These sets may be freely distributed provided they remain properly credited.\"\n\n")
        for k, m in enumerate(lv):
            f.write(f"; {k+1}\n" + '\n'.join(m) + "\n\n")
print("total", total, "max", maxw, maxh, "scroll", big)
