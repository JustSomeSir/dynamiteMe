#!/bin/sh
# Builds the test-only steam_api64.dll proxy with llvm-mingw (x86_64-w64-mingw32-clang++ on PATH).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="${OUT:-$here/out}"
cxx="${CXX:-x86_64-w64-mingw32-clang++}"
mkdir -p "$out"
$cxx -std=c++20 -O2 -Wall -Wextra -Werror -shared -static \
    "$here/proxy.cpp" "$here/p2pcore.cpp" "$here/thunks.S" "$here/exports.def" \
    -o "$out/steam_api64.dll" -lws2_32
echo "built $out/steam_api64.dll"
$cxx -std=c++20 -O1 -Wall -Wextra -Werror -static "$here/selftest.cpp" -o "$out/steamproxy_selftest.exe" -lws2_32
echo "built $out/steamproxy_selftest.exe"
