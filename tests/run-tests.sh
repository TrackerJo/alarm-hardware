#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
binary="$(mktemp -t dawn-core-tests.XXXXXX)"
trap 'rm -f "$binary"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g dawn_core.cpp tests/core_tests.cpp -o "$binary"
"$binary"

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g dawn_core.cpp legacy_protocol.cpp tests/legacy_tests.cpp -o "$binary"
"$binary"

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g -Itests/stubs outputs.cpp tests/outputs_tests.cpp -o "$binary"
"$binary"

"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g dawn_core.cpp wifi_protocol.cpp tests/wifi_tests.cpp -o "$binary"
"$binary"

python3 -m unittest discover -s tests -p 'test_*.py'
