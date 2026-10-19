# pj benchmark suite

Compares pj with yyjson and simdjson (DOM and On-Demand) across five JSON families and a ladder of
document sizes, and publishes the results to the site in `../../site`.

```sh
pj/suite/build.sh                            # runner (same flags and pinned libraries as pj/build.sh)
python3 pj/suite/gen.py pj/suite/data        # 5 families x 1KB..256MB, deterministic (~1.3 GB)
python3 pj/suite/run.py pj/suite/data pj/suite/results/<machine>.json
python3 pj/suite/publish.py                  # results/*.json -> site/data
cd site && vercel deploy --prod
```

Machine ids `publish.py` knows: `c8i`, `c8a`, `c8g` (AWS `.4xlarge`, us-east-1, Ubuntu 24.04,
clang 18). macOS runs work for local checks but are not published.

## What is measured

`runner <parser> <file> [budget_s]` measures one parser on one file in a fresh process:

| field | meaning |
|---|---|
| `cold` | first parse in the process: page faults and first-touch allocation included |
| `cold_faults` | minor page faults during the cold parse |
| `parse` | steady-state parse until the document can be queried (median, min, p25, p75, mean, sd, n) |
| `free` | releasing the document |
| `total` | parse + visit every value + free: the whole cost of using a document once |
| `cpu_parse`, `cpu_total` | CPU time of all threads per document (÷ wall time = cores used) |
| `hw_parse`, `hw_total` | cycles, instructions, branch misses per document; Linux `perf_event` on the measuring thread (single-threaded parsers only), macOS `proc_pid_rusage` (no branch misses) |
| `mem_parse`, `mem_total` | peak memory above the footprint just before the cold parse (and after the walk); Linux `clear_refs` + `VmHWM`, macOS footprint interval max |
| `check` | value count, string/key bytes and number sum of the traversal; must agree across parsers |

Each sample is the mean of enough back-to-back documents to last ≥ 200 µs; a loop runs for the time
budget (default 1 s) with 5 to 1000 samples.

`run.py` runs every (parser, file) 3 times, in a different random order per round, each in a fresh
process, and reports the median across rounds for every number. On Linux, single-threaded parsers are
pinned to CPU 2 with `taskset`. It records the machine (CPU, instance type, kernel, THP mode), the
compilers, flags, library versions, the active simdjson kernel, pj's SIMD layer and git revision.
