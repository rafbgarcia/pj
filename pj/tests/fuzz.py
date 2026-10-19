#!/usr/bin/env python3
"""Generate random JSON docs (valid, plus byte-mutated variants) for difftest.
Usage: fuzz.py <outdir> <count> [seed]"""
import json, os, random, sys

out, count = sys.argv[1], int(sys.argv[2])
rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
os.makedirs(out, exist_ok=True)

KEYS = ["a", "x0", "y0", "id", "name", "k", "long_key_name_here", "é", "☃", 'q"uote', "back\\slash", "", "tab\tkey"]

def rstr():
    r = rnd.random()
    if r < 0.3: return rnd.choice(KEYS)
    n = rnd.choice([0, 1, 5, 6, 7, 8, 15, 16, 17, 40, 300])
    alphabet = "abcXYZ 019,:{}[]\"\\/\n\té€\U0001F600\u0001"
    return "".join(rnd.choice(alphabet) for _ in range(n))

def rnum():
    r = rnd.random()
    if r < 0.2: return rnd.randint(-10, 10)
    if r < 0.35: return rnd.randint(-2**63, 2**64 - 1)
    if r < 0.45: return rnd.choice([0.0, -0.0, 1e-300, 5e-324, 1.7976931348623157e308, 2.2250738585072014e-308])
    if r < 0.8: return rnd.uniform(-180, 180)
    return rnd.uniform(-1, 1) * 10 ** rnd.randint(-30, 30)

budget = [0]
def rval(depth):
    r = rnd.random()
    budget[0] -= 1
    if depth > 6 or r < 0.45 or budget[0] < 0:
        k = rnd.random()
        if k < 0.5: return rnum()
        if k < 0.8: return rstr()
        return rnd.choice([True, False, None])
    if r < 0.7:
        return [rval(depth + 1) for _ in range(rnd.choice([0, 1, 2, 3, 10, 50]))]
    n = rnd.choice([0, 1, 2, 4, 8, 63, 64, 65, 100])
    shape = rnd.random() < 0.6  # repeated shapes exercise the transition caches
    keys = KEYS if shape else None
    o = {}
    for i in range(n):
        k = (keys[i % len(keys)] + str(i // len(keys))) if shape else rstr()
        o[k] = rval(depth + 1)
    return o

def records():
    # array of same-shape objects, like coordinates.json
    return [{"x0": rnum(), "y0": rnum(), "x1": rnum(), "y1": rnum()} if rnd.random() < 0.9 else rval(2) for _ in range(rnd.randint(1, 3000))]


def dump(v):
    style = rnd.random()
    if style < 0.4: return json.dumps(v, separators=(",", ":"), ensure_ascii=rnd.random() < 0.3)
    if style < 0.7: return json.dumps(v, indent=rnd.choice([1, 2, 4]), ensure_ascii=False)
    s = json.dumps(v, ensure_ascii=False)
    return " \n\t" + s + "\r\n "

def raw_numbers():
    nums = ["0", "-0", "1", "-1", "01", "1.", ".5", "1e5", "1E-5", "1e+5", "-1.5e300", "1e400", "-", "--1", "1.5e", "123456789012345678901234567890",
            "0.000000000000000000000000000001234567890123456789", "18446744073709551615", "18446744073709551616", "-9223372036854775808",
            "-9223372036854775809", "140737488355327", "140737488355328", "-140737488355328", "1.7976931348623157e308", "4.9e-324", "2.5", "1.00000000000000000001",
            "9007199254740993", "9007199254740993.0", "0.1", "1e-7", "123.456e-2"]
    return "[" + ",".join(rnd.choice(nums) for _ in range(rnd.randint(1, 50))) + "]"

for i in range(count):
    budget[0] = rnd.choice([50, 500, 5000])
    r = rnd.random()
    if r < 0.4: s = dump(rval(0))
    elif r < 0.7: s = dump(records())
    else: s = raw_numbers()
    b = s.encode("utf-8", "surrogatepass")
    with open(os.path.join(out, f"v{i}.json"), "wb") as f: f.write(b)
    # mutated (mostly invalid) variant
    m = bytearray(b)
    for _ in range(rnd.randint(1, 3)):
        if not m: break
        op, j = rnd.random(), rnd.randrange(len(m))
        if op < 0.4: m[j] = rnd.choice(b'{}[]",:\\0123456789.eE-+ tfn\x00\x80\xc3\xff')
        elif op < 0.7: del m[j]
        else: m.insert(j, rnd.choice(b'{}[]",:\\0123456789 '))
    with open(os.path.join(out, f"m{i}.json"), "wb") as f: f.write(m)
