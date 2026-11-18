#!/bin/sh
# plates_bench: bench.cc linked against out/native's libpdfium.a, written to
# out/native/plates_bench. usage: knowledge/bench/build.sh
set -e
cd "$(dirname "$0")/../.."
: "${SDKROOT:=$HOME/.cache/pdfking-sdk/MacOSX26.1.sdk}"
export SDKROOT
ninja -C out/native pdfium
third_party/llvm-build/Release+Asserts/bin/clang++ -std=c++20 -O2 \
  -isysroot "$SDKROOT" -I. knowledge/bench/bench.cc out/native/obj/libpdfium.a \
  -framework CoreFoundation -framework CoreGraphics -framework AppKit \
  -o out/native/plates_bench
echo out/native/plates_bench
