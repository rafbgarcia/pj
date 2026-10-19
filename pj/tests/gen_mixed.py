#!/usr/bin/env python3
"""Generate mixed.json: 2M {"t":..,"v":..,"w":[..]} objects whose numbers are randomly ints or floats (50/50)."""
import random, sys
out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 2_000_000
rnd = random.Random(11)
def num():
    return str(rnd.randrange(0, 100000)) if rnd.random() < 0.5 else repr(round(rnd.uniform(-1000, 1000), rnd.randrange(1, 8)))
with open(out, 'w') as f:
    f.write('[')
    for i in range(n):
        if i: f.write(',')
        f.write('{"t":%s,"v":%s,"w":[%s,%s,%s]}' % (num(), num(), num(), num(), num()))
    f.write(']')
