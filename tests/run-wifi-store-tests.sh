#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${DAWN_MBEDTLS_ROOT:?Set DAWN_MBEDTLS_ROOT to a built Mbed TLS 3.6.x source directory}"
binary="$(mktemp -t dawn-wifi-store-tests.XXXXXX)"
trap 'rm -f "$binary"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Itests/wifi_stubs -I"$DAWN_MBEDTLS_ROOT/include" wifi_store.cpp tests/wifi_store_tests.cpp \
    "$DAWN_MBEDTLS_ROOT/library/libmbedcrypto.a" -o "$binary"
"$binary"
