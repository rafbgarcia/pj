#!/bin/sh
# Builds $BUILD/bench (default build/) with yyjson + simdjson from ../bench/vendor (fetched if missing).
#   CC/CXX      compilers (default clang/clang++)
#   PJ_MARCH    target flags (default: -mcpu=native on arm64, -march=native on x86-64)
#   PJ_DEFS     extra defines for pj.c, e.g. -DPJ_FORCE_SCALAR
#   BUILD       output dir
set -e
cd "$(dirname "$0")"
CC=${CC:-clang}
CXX=${CXX:-clang++}
BUILD=${BUILD:-build}
V=../bench/vendor
fetch() { # name url sha
    [ -d "$V/$1" ] && return
    git init -q "$V/$1"
    git -C "$V/$1" fetch -q --depth 1 "$2" "$3"
    git -C "$V/$1" checkout -q FETCH_HEAD
}
fetch yyjson https://github.com/ibireme/yyjson a84070fa6dc58f9b379ed75b8c8fab2c1da1b604
fetch simdjson https://github.com/simdjson/simdjson 3c2be3153bb4618d91eccf76f091dd0e8468b8da
if [ -z "$PJ_MARCH" ]; then
    case "$(uname -m)" in
    arm64|aarch64) PJ_MARCH="-mcpu=native" ;;
    *) PJ_MARCH="-march=native" ;;
    esac
fi
mkdir -p "$BUILD"
CFLAGS="-O3 $PJ_MARCH -DNDEBUG"
LIBS=""
[ "$(uname -s)" = Linux ] && LIBS="-lpthread -lm"
[ -f $BUILD/simdjson.o ] || $CXX $CFLAGS -std=c++17 -c $V/simdjson/singleheader/simdjson.cpp -o $BUILD/simdjson.o
[ -f $BUILD/yyjson.o ] || $CC $CFLAGS -c $V/yyjson/src/yyjson.c -o $BUILD/yyjson.o
$CXX $CFLAGS -std=c++17 -I$V/simdjson/singleheader -c sjw.cpp -o $BUILD/sjw.o
$CC $CFLAGS $PJ_DEFS -std=c11 -Wall -Wextra -Wno-unused-label -c pj.c -o $BUILD/pj.o
$CC $CFLAGS -std=c11 -I$V/yyjson/src -c bench.c -o $BUILD/bench.o
$CXX $BUILD/bench.o $BUILD/pj.o $BUILD/sjw.o $BUILD/simdjson.o $BUILD/yyjson.o $LIBS -o $BUILD/bench
