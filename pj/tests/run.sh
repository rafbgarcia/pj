#!/bin/sh
# Correctness: pj vs yyjson full-tree diff on examples, generated data and fuzz docs, across chunk sizes.
# Same env as build.sh (CC, PJ_MARCH, PJ_DEFS, BUILD). Set PJ_RUN to run binaries under a wrapper
# (e.g. "arch -x86_64"); PJ_QUICK=1 skips the large generated files. Exits non-zero on any failure.
set -e
cd "$(dirname "$0")/.."
V=../bench/vendor
CC=${CC:-clang}
BUILD=${BUILD:-build}
./build.sh
if [ -z "$PJ_MARCH" ]; then
    case "$(uname -m)" in arm64|aarch64) PJ_MARCH="-mcpu=native" ;; *) PJ_MARCH="-march=native" ;; esac
fi
LIBS=""
[ "$(uname -s)" = Linux ] && LIBS="-lpthread -lm"
$CC -O2 $PJ_MARCH -I$V/yyjson/src -w tests/difftest.c $BUILD/pj.o $BUILD/yyjson.o $LIBS -o $BUILD/difftest
$CC -O3 $PJ_MARCH -DNDEBUG -DPJ_POISON $PJ_DEFS -std=c11 -w -c pj.c -o $BUILD/pj_poison.o
$CC -O2 $PJ_MARCH -I$V/yyjson/src -w tests/difftest.c $BUILD/pj_poison.o $BUILD/yyjson.o $LIBS -o $BUILD/difftest_poison
# the other window mode than this target's default (windowed soft stops vs one window per chunk)
for wv in 0 1; do
  $CC -O3 $PJ_MARCH -DNDEBUG -DPJ_WINDOWS=$wv $PJ_DEFS -std=c11 -w -c pj.c -o $BUILD/pj_w$wv.o
  $CC -O2 $PJ_MARCH -I$V/yyjson/src -w tests/difftest.c $BUILD/pj_w$wv.o $BUILD/yyjson.o $LIBS -o $BUILD/difftest_w$wv
done
D=build # generated data is arch-independent; share it
[ -d $D/data ] || python3 tests/gen.py $D/data
for s in 1 2 3; do [ -d $D/fuzz$s ] || python3 tests/fuzz.py $D/fuzz$s 400 $s; done
FILES="$V/simdjson/jsonexamples/*.json $D/fuzz*/*.json"
BIG="$D/data/*.json"
[ -f ../coordinates.json ] && BIG="$BIG ../coordinates.json"
[ -n "$PJ_QUICK" ] && BIG="" # emulators: skip the large files
fail=0
check() { # label, cmd... (the command's own exit status: a crash must fail, not just the last line)
    label=$1; shift
    rc=0; "$@" > $BUILD/check.out 2>/dev/null || rc=$?
    if [ $rc = 0 ]; then echo "$label: $(tail -1 $BUILD/check.out)"
    else echo "$label: $(tail -1 $BUILD/check.out)  <-- FAIL (exit $rc)"; fail=1; fi
}
echo "arch: $($PJ_RUN $BUILD/difftest --arch 2>/dev/null || true)"
check "default chunks" $PJ_RUN $BUILD/difftest $FILES $BIG
for c in 7 33 4096; do check "PJ_CHUNK=$c" env PJ_CHUNK=$c $PJ_RUN $BUILD/difftest $FILES; done
check "poisoned recycled memory, PJ_CHUNK=7" env PJ_CHUNK=7 $PJ_RUN $BUILD/difftest_poison $FILES
check "single thread" env PJ_THREADS=1 $PJ_RUN $BUILD/difftest $FILES
for wv in 0 1; do
  check "PJ_WINDOWS=$wv" $PJ_RUN $BUILD/difftest_w$wv $FILES
  for c in 64 256 4096; do check "PJ_WINDOWS=$wv, single thread, PJ_WIN=$c" env PJ_THREADS=1 PJ_WIN=$c $PJ_RUN $BUILD/difftest_w$wv $FILES; done
  check "PJ_WINDOWS=$wv, PJ_CHUNK=33, PJ_WIN=64" env PJ_CHUNK=33 PJ_WIN=64 $PJ_RUN $BUILD/difftest_w$wv $FILES
done
for c in 7 33; do check "PJ_CHUNK=$c, wrong boundary guesses" env PJ_SPECFLIP=1 PJ_CHUNK=$c $PJ_RUN $BUILD/difftest $FILES; done
check "wrong boundary guesses, large files" env PJ_SPECFLIP=1 $PJ_RUN $BUILD/difftest $BIG
for c in 7 33; do check "PJ_CHUNK=$c, exact boundaries" env PJ_NOSPEC=1 PJ_CHUNK=$c $PJ_RUN $BUILD/difftest $FILES; done
check "single thread, poisoned, PJ_WIN=64" env PJ_THREADS=1 PJ_WIN=64 $PJ_RUN $BUILD/difftest_poison $FILES
check "single thread, large files" env PJ_THREADS=1 $PJ_RUN $BUILD/difftest $BIG
exit $fail
