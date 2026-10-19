#!/bin/sh
# Builds suite/build/runner (same flags and pinned yyjson/simdjson as ../build.sh).
set -e
cd "$(dirname "$0")"
[ -d ../../bench/vendor/yyjson ] || BUILD=build ../build.sh >/dev/null
CC=${CC:-clang}
CXX=${CXX:-clang++}
V=../../bench/vendor
if [ -z "$PJ_MARCH" ]; then
    case "$(uname -m)" in
    arm64|aarch64) PJ_MARCH="-mcpu=native" ;;
    *) PJ_MARCH="-march=native" ;;
    esac
fi
mkdir -p build
CFLAGS="-O3 $PJ_MARCH -DNDEBUG"
LIBS=""
[ "$(uname -s)" = Linux ] && LIBS="-lpthread -lm"
[ -f build/simdjson.o ] || $CXX $CFLAGS -std=c++17 -c $V/simdjson/singleheader/simdjson.cpp -o build/simdjson.o
[ -f build/yyjson.o ] || $CC $CFLAGS -c $V/yyjson/src/yyjson.c -o build/yyjson.o
$CXX $CFLAGS -std=c++17 -I$V/simdjson/singleheader -c sj.cpp -o build/sj.o
$CC $CFLAGS -std=c11 -c ../pj.c -o build/pj.o
$CC $CFLAGS -std=c11 -I.. -I$V/yyjson/src -c runner.c -o build/runner.o
$CXX build/runner.o build/pj.o build/sj.o build/simdjson.o build/yyjson.o $LIBS -o build/runner
