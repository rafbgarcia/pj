#!/usr/bin/env python3
"""Generate larger general-purpose benchmark files into <outdir>."""
import json, random, sys, os
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
J = os.path.join(os.path.dirname(__file__), '../../bench/vendor/simdjson/jsonexamples')
rnd = random.Random(42)
tw = json.load(open(os.path.join(J, 'twitter.json')))
json.dump([tw] * 150, open(os.path.join(out, 'twitter_x150.json'), 'w'), ensure_ascii=False)
ci = json.load(open(os.path.join(J, 'citm_catalog.json')))
json.dump([ci] * 50, open(os.path.join(out, 'citm_x50.json'), 'w'), ensure_ascii=False, indent=2)
# canada.json-like: GeoJSON polygons, arrays of [lon, lat] pairs
feat = {"type": "FeatureCollection", "features": [{"type": "Feature", "properties": {"name": "Canada"}, "geometry": {"type": "Polygon",
        "coordinates": [[[round(rnd.uniform(-141, -52), 15), round(rnd.uniform(41, 84), 15)] for _ in range(20000)] for _ in range(200)]}}]}
json.dump(feat, open(os.path.join(out, 'canada_like.json'), 'w'), separators=(',', ':'))
# mixed records with strings, ints, bools, nested arrays
recs = [{"id": i, "uuid": "%032x" % rnd.getrandbits(128), "name": "user_%d" % rnd.randint(0, 10**6), "active": rnd.random() < 0.5,
         "score": rnd.uniform(0, 100), "tags": ["t%d" % rnd.randint(0, 50) for _ in range(rnd.randint(0, 5))],
         "geo": {"lat": rnd.uniform(-90, 90), "lon": rnd.uniform(-180, 180)}} for i in range(1_000_000)]
json.dump(recs, open(os.path.join(out, 'records.json'), 'w'), separators=(',', ':'))
