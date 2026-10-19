#!/usr/bin/env python3
"""Run every parser on every ladder file; write one results JSON per machine.

Usage: run.py <datadir> <out.json> [--budget S] [--rounds N] [--sizes 1KB,4MB] [--families a,b] [--parsers a,b]

Method:
  - one fresh process per (parser, file, round): cold numbers and peak memory are clean
  - 3 rounds run the parsers in a different random order (seeded), so drift and thermal state
    do not favour a fixed position; per metric, the reported value is the median across rounds
  - Linux: single-threaded parsers are pinned to one core (taskset); pj with all cores is not
  - every parser's traversal checksum (value count, string bytes, number sum) must agree
"""
import argparse, json, os, platform, random, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(HERE, 'build/runner')
PARSERS = ['yyjson', 'simdjson', 'simdjson-od', 'pj-1t', 'pj']
PIN_CPU = '2'  # avoid CPU 0 (interrupts, housekeeping)


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:
        return ''


def imds(path):  # EC2 instance metadata (IMDSv2)
    tok = sh("curl -s -m 1 -X PUT http://169.254.169.254/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 60'")
    if not tok:
        return ''
    return sh("curl -s -m 1 -H 'X-aws-ec2-metadata-token: %s' http://169.254.169.254/latest/meta-data/%s" % (tok, path))


def machine():
    m = {'os': platform.platform(), 'kernel': platform.release(), 'arch': platform.machine(), 'logical_cpus': os.cpu_count()}
    if sys.platform == 'darwin':
        m['cpu'] = sh('sysctl -n machdep.cpu.brand_string')
        m['mem_bytes'] = int(sh('sysctl -n hw.memsize') or 0)
        m['cores'] = '%s performance + %s efficiency' % (sh('sysctl -n hw.perflevel0.physicalcpu'), sh('sysctl -n hw.perflevel1.physicalcpu'))
    else:
        m['cpu'] = sh("lscpu | sed -n 's/^Model name: *//p' | head -1") or platform.processor()
        m['mem_bytes'] = int(sh("awk '/MemTotal/{print $2*1024}' /proc/meminfo") or 0)
        m['threads_per_core'] = sh("lscpu | sed -n 's/^Thread(s) per core: *//p'")
        m['instance_type'] = imds('instance-type')
        m['availability_zone'] = imds('placement/availability-zone')
        m['thp'] = sh('cat /sys/kernel/mm/transparent_hugepage/enabled')
        m['governor'] = sh('cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null') or 'n/a (VM)'
        m['os'] = sh(". /etc/os-release && echo $PRETTY_NAME") or m['os']
    return m


def env():
    e = {'cc': sh('${CC:-clang} --version | head -1'), 'cxx': sh('${CXX:-clang++} --version | head -1'),
         'cflags': '-O3 %s -DNDEBUG' % ('-mcpu=native' if platform.machine() in ('arm64', 'aarch64') else '-march=native'),
         'git': sh('git -C %s rev-parse --short HEAD' % HERE), 'dirty': bool(sh('git -C %s status --porcelain -- ..' % HERE)),
         'date': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    try:
        e['versions'] = json.loads(sh(RUNNER + ' --versions'))
    except Exception:
        e['versions'] = {}
    return e


def run_one(parser, path, budget):
    cmd = [RUNNER, parser, path, str(budget)]
    if sys.platform.startswith('linux') and parser != 'pj' and shutil.which('taskset'):
        cmd = ['taskset', '-c', PIN_CPU] + cmd
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()
    return json.loads(r.stdout), None


def med(xs):
    xs = sorted(x for x in xs if x is not None)
    return xs[len(xs) // 2] if xs else None


def combine(runs):
    """Median across rounds for every numeric leaf (rounds are independent processes)."""
    out = json.loads(json.dumps(runs[0]))
    def walk(dst, srcs):
        for k, v in dst.items():
            if isinstance(v, dict):
                walk(v, [s.get(k) or {} for s in srcs])
            elif isinstance(v, (int, float)) and not isinstance(v, bool) and k not in ('bytes', 'threads'):
                dst[k] = med([s.get(k) for s in srcs])
    walk(out, runs)
    out['rounds'] = len(runs)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data'); ap.add_argument('out')
    ap.add_argument('--budget', type=float, default=1.0, help='seconds per measurement loop')
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--sizes'); ap.add_argument('--families'); ap.add_argument('--parsers')
    a = ap.parse_args()
    meta = json.load(open(os.path.join(a.data, 'meta.json')))
    sizes = a.sizes.split(',') if a.sizes else meta['sizes']
    fams = a.families.split(',') if a.families else list(meta['families'])
    parsers = a.parsers.split(',') if a.parsers else PARSERS
    res = {'machine': machine(), 'env': env(), 'method': {'budget_s': a.budget, 'rounds': a.rounds, 'pin_cpu': PIN_CPU},
           'sizes': sizes, 'families': {f: meta['families'][f] for f in fams}, 'parsers': parsers, 'results': [], 'failures': []}
    print(json.dumps(res['machine']), json.dumps(res['env']), sep='\n', flush=True)
    rng = random.Random(1)
    for size in sizes:
        for fam in fams:
            path = os.path.join(a.data, '%s_%s.json' % (fam, size))
            runs = {p: [] for p in parsers}
            for r in range(a.rounds):
                order = parsers[:]
                rng.shuffle(order)
                for p in order:
                    v, err = run_one(p, path, a.budget)
                    if v is None:
                        res['failures'].append({'parser': p, 'family': fam, 'size': size, 'error': err})
                        print('FAIL', p, path, err, flush=True)
                    else:
                        runs[p].append(v)
            rows = {p: combine(v) for p, v in runs.items() if v}
            checks = {json.dumps(v['check'], sort_keys=True) for v in rows.values()}
            ok = len(checks) == 1
            for p, v in rows.items():
                v.update(family=fam, size=size, checksum_ok=ok)
                res['results'].append(v)
            line = '  '.join('%s %.3f/%.3fms' % (p, v['parse']['med'] / 1e6, v['total']['med'] / 1e6) for p, v in rows.items())
            print('%-12s %-6s %s%s' % (fam, size, line, '' if ok else '  CHECKSUM MISMATCH'), flush=True)
            tmp = a.out + '.tmp'
            json.dump(res, open(tmp, 'w'))
            os.replace(tmp, a.out)


if __name__ == '__main__':
    main()
