#!/bin/sh
# Host-side tests for the hook (no game, no Windows). Needs g++ and the flatbuffers headers that the
# hook's CMake build fetches (pass their include dir as FLATBUFFERS_INCLUDE, or build the hook once).
# Runs each test normally, then once per mutant: a test that still passes on a mutant is useless.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../src"
fb="${FLATBUFFERS_INCLUDE:-$(find "$here/.." -path '*_deps/flatbuffers-src/include' -type d | head -1)}"
[ -n "$fb" ] || { echo "flatbuffers headers not found, set FLATBUFFERS_INCLUDE"; exit 2; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
CXX="${CXX:-g++}"
flags="-std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I $src -I $src/DynamiteSyncImpl -I $fb"

$CXX $flags "$here/message_validation_test.cpp" -o "$out/mv"
"$out/mv"
$CXX $flags "$here/instance_name_test.cpp" -o "$out/inst"
"$out/inst"
$CXX $flags "$here/memscan_test.cpp" -o "$out/memscan"
"$out/memscan"
# memscan mutants: no visited set check on depth, radius ignored on one axis
for m in 's|if (node.depth >= o.depth) continue;||' 's/ || std::fabs(v\[2\] - o.z) > o.radius//'; do
    mkdir -p "$out/ms/tests" "$out/ms/src"
    sed "$m" "$src/MemScanCore.h" > "$out/ms/src/MemScanCore.h"
    cmp -s "$src/MemScanCore.h" "$out/ms/src/MemScanCore.h" && { echo "memscan mutant did not apply: $m"; exit 1; }
    cp "$here/memscan_test.cpp" "$out/ms/tests/"
    $CXX $flags "$out/ms/tests/memscan_test.cpp" -o "$out/ms/t"
    if "$out/ms/t" > /dev/null 2>&1; then echo "memscan MUTANT SURVIVED: $m"; exit 1; else echo "memscan mutant killed"; fi
done

# Mutants: break the validator in ways a real regression could, expect the test to fail.
mutants=0; killed=0
for m in \
    's|return "message body missing";|return nullptr;|' \
    's|v->size() >= need|v->size() >= 1|g' \
    's|return w->msg_as_BodyPosition()->pos() == nullptr ? "BodyPosition: pos missing" : nullptr;|return nullptr;|' \
    's|> UINT32_MAX|> UINT64_MAX|' \
    's|if (m->category() == nullptr \|\| m->name() == nullptr) {|if (m->category() == nullptr) {|g'
do
    mutants=$((mutants+1))
    mkdir -p "$out/m$mutants/DynamiteSyncImpl"
    sed "$m" "$src/DynamiteSyncImpl/MessageValidation.h" > "$out/m$mutants/DynamiteSyncImpl/MessageValidation.h"
    if cmp -s "$src/DynamiteSyncImpl/MessageValidation.h" "$out/m$mutants/DynamiteSyncImpl/MessageValidation.h"; then
        echo "mutant $mutants did not apply: $m"; exit 1
    fi
    cp "$src/DynamiteSyncImpl/DynamiteSyncSchema_generated.h" "$out/m$mutants/DynamiteSyncImpl/"
    $CXX -I "$out/m$mutants" -I "$out/m$mutants/DynamiteSyncImpl" $flags -Wno-type-limits -Wno-tautological-compare -Wno-unused-parameter "$here/message_validation_test.cpp" -o "$out/m$mutants/mv" 2>/dev/null \
        || { echo "mutant $mutants: does not compile (counts as killed)"; killed=$((killed+1)); continue; }
    if "$out/m$mutants/mv" > /dev/null 2>&1; then echo "mutant $mutants SURVIVED: $m"; else killed=$((killed+1)); fi
done
echo "mutants killed: $killed/$mutants"
[ "$killed" -eq "$mutants" ]
