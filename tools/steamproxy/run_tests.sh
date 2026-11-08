#!/bin/sh
# Host tests of the transport core (Linux, g++). ASan/UBSan/LSan run, then TSan 3 times.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined,leak -g -pthread "$here/p2pcore.cpp" "$here/test_p2pcore.cpp" -o "$out/t"
"$out/t"
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=thread -g -pthread "$here/p2pcore.cpp" "$here/test_p2pcore.cpp" -o "$out/tt"
for i in 1 2 3; do TSAN_OPTIONS=halt_on_error=1 "$out/tt" > /dev/null; done
echo "tsan: 3 clean runs"
