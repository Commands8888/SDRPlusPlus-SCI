#!/bin/bash
# Build the regression harness against the exact patched dependency source.
set -euo pipefail
source_dir=${1:?Usage: test_rtl_async_teardown.sh RTL_SDR_SOURCE_DIR}
repo_root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O1 -g -ffunction-sections -fdata-sections -Drtlsdr_STATIC \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    $(pkg-config --cflags libusb-1.0) \
    -I"$source_dir/include" -I"$source_dir/src" \
    "$repo_root/tests/rtl_async_teardown.c" \
    "$source_dir"/src/tuner_*.c \
    -Wl,--gc-sections $(pkg-config --libs libusb-1.0) -lm -pthread \
    -o "$tmp/rtl-async-test"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$tmp/rtl-async-test"
