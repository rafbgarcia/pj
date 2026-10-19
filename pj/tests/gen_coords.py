#!/usr/bin/env python3
"""Generate coordinates.json-style data: [{"x0":..,"y0":..,"x1":..,"y1":..}, ...] (5M items by default)."""
import random, sys
out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 5_000_000
rnd = random.Random(7)
u = rnd.uniform
with open(out, 'w') as f:
    f.write('[')
    for i in range(n):
        if i: f.write(',')
        f.write('{"x0":%r,"y0":%r,"x1":%r,"y1":%r}' % (u(0, 180), u(0, 90), u(0, 180), u(0, 90)))
    f.write(']')
