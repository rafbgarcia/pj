#!/usr/bin/env python3
"""Generate the size ladder: <outdir>/<family>_<size>.json for every family x size.

Each file is prefix + elements joined by ',' + suffix, filled until the target size
(at least one element). Elements cycle through a deterministic pool, so files are
identical on every machine.
Usage: gen.py <outdir> [max_size_label]
"""
import json, os, random, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EX = os.path.join(HERE, '../../bench/vendor/simdjson/jsonexamples')
SIZES = [('1KB', 1 << 10), ('16KB', 16 << 10), ('256KB', 256 << 10), ('4MB', 4 << 20),
         ('256MB', 256 << 20)]
POOL = 20000


def coordinates():
    r = random.Random(7); u = r.uniform
    els = ['{"x0":%r,"y0":%r,"x1":%r,"y1":%r}' % (u(0, 180), u(0, 90), u(0, 180), u(0, 90)) for _ in range(POOL)]
    return '[', els, ']'


def records():
    r = random.Random(42)
    els = [json.dumps({"id": i, "uuid": "%032x" % r.getrandbits(128), "name": "user_%d" % r.randint(0, 10**6),
                       "active": r.random() < 0.5, "score": r.uniform(0, 100),
                       "tags": ["t%d" % r.randint(0, 50) for _ in range(r.randint(0, 5))],
                       "geo": {"lat": r.uniform(-90, 90), "lon": r.uniform(-180, 180)}}, separators=(',', ':'))
           for i in range(POOL)]
    return '[', els, ']'


def twitter():
    tw = json.load(open(os.path.join(EX, 'twitter.json')))
    els = [json.dumps(s, ensure_ascii=False, separators=(',', ':')) for s in tw['statuses']]
    return '{"statuses":[', els, '],"search_metadata":' + json.dumps(tw['search_metadata'], separators=(',', ':')) + '}'


def canada():
    r = random.Random(3)
    els = ['[%r,%r]' % (round(r.uniform(-141, -52), 15), round(r.uniform(41, 84), 15)) for _ in range(POOL)]
    pre = '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"Canada"},"geometry":{"type":"Polygon","coordinates":[['
    return pre, els, ']]}}]}'


def citm():
    ci = json.load(open(os.path.join(EX, 'citm_catalog.json')))
    els = [json.dumps(p, ensure_ascii=False, indent=2).replace('\n', '\n    ') for p in ci['performances']]
    return '{\n  "performances": [\n    ', els, '\n  ]\n}\n'


FAMILIES = {
    'coordinates': (coordinates, 'Numbers: {"x0","y0","x1","y1"} float objects'),
    'records': (records, 'Mixed records: ints, floats, strings, bools, nested object and array'),
    'twitter': (twitter, 'Real tweets (twitter.json statuses): string- and unicode-heavy, deep'),
    'canada': (canada, 'GeoJSON polygon: arrays of [lon, lat] float pairs'),
    'citm': (citm, 'citm_catalog performances, pretty-printed (indent 2): ints and whitespace'),
}


def write(path, pre, els, suf, target, sep=','):
    if os.path.exists(path):
        return
    budget = target - len(pre.encode()) - len(suf.encode())
    enc = [e.encode() for e in els]
    sepb = (',\n    ' if '\n' in pre else sep).encode()
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(pre.encode())
        n, i = 0, 0
        buf = []
        while True:
            e = enc[i % len(enc)]
            add = len(e) + (len(sepb) if i else 0)
            if i and n + add > budget:
                break
            if i: buf.append(sepb)
            buf.append(e); n += add; i += 1
            if len(buf) > 8192:
                f.write(b''.join(buf)); buf = []
        f.write(b''.join(buf))
        f.write(suf.encode())
    os.rename(tmp, path)


def main():
    out = sys.argv[1]
    maxlabel = sys.argv[2] if len(sys.argv) > 2 else '256MB'
    os.makedirs(out, exist_ok=True)
    sizes = SIZES[:[s for s, _ in SIZES].index(maxlabel) + 1]
    meta = {'sizes': [s for s, _ in sizes], 'families': {k: d for k, (_, d) in FAMILIES.items()}}
    for fam, (fn, _) in FAMILIES.items():
        pre, els, suf = fn()
        for label, target in sizes:
            write(os.path.join(out, '%s_%s.json' % (fam, label)), pre, els, suf, target)
            print(fam, label, flush=True)
    json.dump(meta, open(os.path.join(out, 'meta.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
