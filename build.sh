#!/usr/bin/env bash
# Plain g++ build, no cmake required.
# Produces build/libaulos.a, build/libaulos.so, build/test_aulos, build/bench_aulos,
# build/aulos_demo.
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Iinclude -Iextern"
CFLAGS="-O2 -Iextern"

echo "[1/7] miniaudio"
cc $CFLAGS -c src/miniaudio_impl.c -o build/miniaudio_impl.o

echo "[2/7] aulos"
c++ $CXXFLAGS -c src/aulos.cpp -o build/aulos.o

echo "[3/7] archive"
ar rcs build/libaulos.a build/aulos.o build/miniaudio_impl.o

echo "[4/7] tests"
c++ $CXXFLAGS tests/test_aulos.cpp -o build/test_aulos build/libaulos.a -lpthread -lm -ldl

echo "[5/7] benchmark"
c++ $CXXFLAGS tests/bench_aulos.cpp -o build/bench_aulos build/libaulos.a -lpthread -lm -ldl

echo "[6/7] offline demo renderer"
c++ $CXXFLAGS tools/aulos_demo.cpp -o build/aulos_demo build/libaulos.a -lpthread -lm -ldl

echo "[7/7] shared library for Unity / C# hosts"
c++ $CXXFLAGS -fPIC -shared -DAULOS_BUILD_SHARED src/aulos.cpp src/miniaudio_impl.c \
    -o build/libaulos.so -lpthread -lm -ldl

echo
echo "built: build/libaulos.a, build/libaulos.so, build/test_aulos, build/bench_aulos, build/aulos_demo"
