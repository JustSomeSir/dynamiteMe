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

# steamshim.dll: same code in SP_HOOK_MODE, detours the loaded steam_api64.dll with MinHook.
# MINHOOK_SRC = a MinHook checkout (the hook's CMake build fetches one into _deps/minhook-src).
if [ -n "${MINHOOK_SRC:-}" ]; then
    cc="${CC:-x86_64-w64-mingw32-clang}"
    mkdir -p "$out/mh"
    for f in "$MINHOOK_SRC"/src/buffer.c "$MINHOOK_SRC"/src/hook.c "$MINHOOK_SRC"/src/trampoline.c "$MINHOOK_SRC"/src/hde/hde64.c; do
        $cc -O2 -c "$f" -I "$MINHOOK_SRC/include" -o "$out/mh/$(basename "$f" .c).o"
    done
    $cxx -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-function -shared -static -DSP_HOOK_MODE -I "$MINHOOK_SRC/include" \
        "$here/proxy.cpp" "$here/p2pcore.cpp" "$here/thunks.S" "$out"/mh/*.o -o "$out/steamshim.dll" -lws2_32
    echo "built $out/steamshim.dll"
else
    echo "MINHOOK_SRC not set: steamshim.dll skipped"
fi
