#!/bin/sh
# Builds instancediag dinput8.dll with llvm-mingw. MINHOOK_SRC = a MinHook checkout
# (the hook's CMake build fetches one into _deps/minhook-src).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mh="${MINHOOK_SRC:?set MINHOOK_SRC to a MinHook source dir}"
cxx="${CXX:-x86_64-w64-mingw32-clang++}"
cc="${CC:-x86_64-w64-mingw32-clang}"
out="${OUT:-$here/out}"
mkdir -p "$out/obj"
for f in "$mh"/src/buffer.c "$mh"/src/hook.c "$mh"/src/trampoline.c "$mh"/src/hde/hde64.c; do
    $cc -O2 -c "$f" -I "$mh/include" -o "$out/obj/$(basename "$f" .c).o"
done
$cxx -std=c++20 -O2 -Wall -Wextra -Werror -shared -static -I "$mh/include" "$here/instancediag.cpp" "$out"/obj/*.o \
    -o "$out/dinput8.dll" -Wl,--kill-at -luser32
echo "built $out/dinput8.dll"
