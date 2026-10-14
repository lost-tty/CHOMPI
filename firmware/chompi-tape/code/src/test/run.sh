#!/bin/sh
# Builds and runs the host tests with clang on the Mac. A quoted include looks next to the including
# file first, so the headers under test sit in build/inc as links, next to the stubs
set -e
cd "$(dirname "$0")"
json=../../libs/coreJSON/source
mkdir -p build/inc
for h in PresetManager.h; do ln -sf ../../../$h build/inc/$h; done
ln -sf ../../stubs/FileStreamingManager.h build/inc/FileStreamingManager.h
clang -std=c11 -O1 -I$json/include -c $json/core_json.c -o build/core_json.o
clang++ -std=c++17 -O1 -Wall -Wno-unknown-pragmas -Ibuild/inc -I$json/include preset_test.cpp build/core_json.o \
    -o build/preset_test
./build/preset_test
