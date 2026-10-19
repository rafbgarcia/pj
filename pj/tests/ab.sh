#!/bin/bash
# A/B timing: tests/ab.sh <rounds> <threads> "name|src.c|defines" ... -- builds each variant (its own
# pj.c, extra -D flags) with tests/prof.c and interleaves runs so drift hits all of them. Prints the
# best-of-rounds time per file relative to the first variant. List the base twice (A ... A2): A vs A2
# is the noise floor of that run. Single-threaded runs are pinned to CPU 2 on Linux.
#   tests/ab.sh 5 1 "A|base.c|" "new|pj.c|" "flag|pj.c|-DPJ_FOO=1" "A2|base.c|"
# AB_FILES overrides the file list.
set -e
cd "$(dirname "$0")/.."
R=$1; NT=$2; shift 2
CC=${CC:-clang}
if [ -z "$PJ_MARCH" ]; then case "$(uname -m)" in arm64|aarch64) PJ_MARCH="-mcpu=native" ;; *) PJ_MARCH="-march=native" ;; esac; fi
LIBS=""; PIN=""
[ "$(uname -s)" = Linux ] && LIBS="-lpthread -lm" && [ "$NT" = 1 ] && PIN="taskset -c 2"
names=()
for v in "$@"; do
  IFS='|' read -r n src defs <<< "$v"; names+=("$n")
  d=build/ab_$n; mkdir -p "$d"; cp "$src" "$d/pj.c"; cp pj.h pow5.h arch_*.h "$d/" # variants share the current headers
  $CC -O3 $PJ_MARCH -DNDEBUG -w $defs tests/prof.c "$d/pj.c" -I"$d" $LIBS -o "build/prof_$n"
done
FILES=${AB_FILES:-"../bench/vendor/simdjson/jsonexamples/twitter.json ../bench/vendor/simdjson/jsonexamples/citm_catalog.json build/data/twitter_x150.json build/data/citm_x50.json build/data/records.json build/data/canada_like.json ../coordinates.json"}
printf "%-20s" file; for n in "${names[@]}"; do printf "%10s" "$n"; done; echo
for f in $FILES; do
  [ -f "$f" ] || continue
  sz=$(wc -c < "$f"); reps=$(( 400000000 / (sz + 1) + 3 )); [ $reps -gt 2000 ] && reps=2000
  best=(); for k in "${!names[@]}"; do best[$k]=999999; done # indexed arrays: macOS bash 3.2
  for i in $(seq "$R"); do for k in "${!names[@]}"; do
    t=$($PIN "build/prof_${names[$k]}" "$f" $reps "$NT" | sed 's/.*best \([0-9.]*\) ms.*/\1/')
    best[$k]=$(awk -v a="${best[$k]}" -v b="$t" 'BEGIN{print (b<a)?b:a}')
  done; done
  b0=${best[0]}
  printf "%-20s" "$(basename "$f" .json)"
  for k in "${!names[@]}"; do awk -v a="$b0" -v b="${best[$k]}" 'BEGIN{printf "%10.3f", b/a}'; done
  printf "   (%s ms)\n" "$b0"
done
