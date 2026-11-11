#!/usr/bin/env python3
"""Checks solutions saved from the web against our levels and our proven counts.

Reads every .htm/.html page in data/online/ with tables of solutions (a caption with the set name,
then rows of level, moves, pushes, solution), replays each solution, and writes the valid ones to
data/online-best.txt (set|level|moves|pushes|solution|source), keeping the fewest moves and the fewest
pushes found for each level. Then it reports:
  - any online solution with fewer moves or pushes than a count we call proven (that would be a bug),
  - for each level we haven't proven, the range the fewest must lie in (solver lower bound .. best
    solution known)."""
import html, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
src = open(os.path.join(ROOT, 'tools', 'make_fewest.py')).read().replace('\nmain()\n', '\n')
ns = {'__file__': os.path.join(ROOT, 'tools', 'make_fewest.py')}
exec(src, ns)
sets = ns['load_sets']()
replay = ns['replay']


def parse_page(text):
    out = []
    for cap, body in re.findall(r'<CAPTION[^>]*>(.*?)</CAPTION>(.*?)</TABLE>', text, re.S | re.I):
        name = html.unescape(re.sub('<[^>]+>', '', cap)).strip()
        for row in re.findall(r'<TR>(.*?)</TR>', body, re.S | re.I):
            cells = [html.unescape(re.sub('<[^>]+>', '', c)).strip() for c in re.findall(r'<TD[^>]*>(.*?)</TD>', row, re.S | re.I)]
            if len(cells) >= 4 and cells[0].isdigit():
                out.append((name, int(cells[0]), re.sub(r'\s+', '', cells[3])))
    return out


def read_data(fn):
    d = {}
    p = os.path.join(ROOT, 'data', fn)
    if os.path.exists(p):
        for line in open(p):
            if line[0] != '#' and line.strip():
                a = line.rstrip('\n').split('|')
                d[(a[0], int(a[1]))] = (int(a[2]), int(a[3]))
    return d


def lower_bounds():
    lb = {}
    p = os.path.join(ROOT, 'data', 'fewest-failed.txt')
    if os.path.exists(p):
        for line in open(p):
            m = re.match(r'(.*?)\|(\d+)\|.*\|(v[0-9]|p[0-9]) .*at least (\d+) (moves|pushes)', line)
            if m:
                k = (m[1], int(m[2]), m[5])
                lb[k] = max(lb.get(k, 0), int(m[4]))
    return lb


def main():
    folder = os.path.join(ROOT, 'data', 'online')
    best = {}                      # (set, lv, 'moves'|'pushes') -> (moves, pushes, sol, source)
    bad = n = 0
    for fn in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if not fn.lower().endswith(('.htm', '.html')):
            continue
        for name, lv, sol in parse_page(open(os.path.join(folder, fn), encoding='utf-8', errors='replace').read()):
            if name not in sets or not 1 <= lv <= len(sets[name]):
                continue
            n += 1
            r = replay(sets[name][lv - 1], sol)
            if not r:
                bad += 1
                continue
            for kind, key in (('moves', r), ('pushes', (r[1], r[0]))):
                old = best.get((name, lv, kind))
                okey = None if not old else ((old[0], old[1]) if kind == 'moves' else (old[1], old[0]))
                if not old or key < okey:
                    best[(name, lv, kind)] = (r[0], r[1], sol, fn)
    with open(os.path.join(ROOT, 'data', 'online-best.txt'), 'w') as f:
        f.write('# Solutions found online (data/online/), checked by tools/check_online.py: the fewest moves and\n'
                '# the fewest pushes found for each level. Not proven optimal. set|level|moves|pushes|solution|source\n')
        seen = set()
        for (name, lv, kind), (m, p, sol, src_) in sorted(best.items()):
            if (name, lv, sol) not in seen:
                seen.add((name, lv, sol))
                f.write(f'{name}|{lv}|{m}|{p}|{sol}|{src_}\n')
    print(f'{n} online solutions read, {n - bad} solve their level, {bad} do not')
    fm, fp, lb = read_data('fewest.txt'), read_data('fewest-pushes.txt'), lower_bounds()
    for (name, lv, kind), (m, p, sol, src_) in sorted(best.items()):
        if kind == 'moves' and (name, lv) in fm and m < fm[(name, lv)][0]:
            print(f'!! {name} {lv}: online {m} moves beats our proven {fm[(name, lv)][0]}')
        if kind == 'pushes' and (name, lv) in fp and p < fp[(name, lv)][1]:
            print(f'!! {name} {lv}: online {p} pushes beats our proven {fp[(name, lv)][1]}')
    # ranges for what's not proven: upper bound = best online or our own solutions
    print('\nNot proven yet (fewest is somewhere in this range):')
    for name in sets:
        if not name.startswith('Microban'):
            continue
        for lv in range(1, len(sets[name]) + 1):
            for kind in ('moves', 'pushes'):
                have = fm if kind == 'moves' else fp
                if (name, lv) in have:
                    continue
                ups = []
                o = best.get((name, lv, kind))
                if o: ups.append((o[0] if kind == 'moves' else o[1], 'online'))
                if kind == 'moves' and (name, lv) in fp: ups.append((fp[(name, lv)][0], 'our pushes solution'))
                if kind == 'pushes' and (name, lv) in fm: ups.append((fm[(name, lv)][1], 'our moves solution'))
                up = min(ups) if ups else None
                lo = lb.get((name, lv, kind))
                print(f'  {name} {lv} {kind}: at least {lo if lo else "?"}, at most '
                      f'{str(up[0]) + " (" + up[1] + ")" if up else "?"}'
                      f'{"  <- PROVEN" if up and lo and up[0] == lo else ""}')


main()
