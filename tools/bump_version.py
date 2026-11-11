#!/usr/bin/env python3
"""Add 0.01 to the version in VERSION (0.5 -> 0.51 -> ... -> 0.99 -> 1.0) and update the README heading.
Run once per build that goes out: python3 tools/bump_version.py"""
import os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
path = os.path.join(root, 'VERSION')
v = open(path).read().strip()
hundredths = round(float(v) * 100) + (0 if '--show' in sys.argv else 1)
new = f"{hundredths // 100}.{hundredths % 100:02d}".rstrip('0')
if new.endswith('.'): new += '0'
open(path, 'w').write(new + '\n')
readme = os.path.join(root, 'README.md')
s = open(readme).read()
s = re.sub(r'## Features \(v[0-9.]+\)', f'## Features (v{new})', s)
open(readme, 'w').write(s)
print(new)
