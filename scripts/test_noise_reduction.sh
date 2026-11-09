#!/bin/bash
# Bounded numerical, buffering and lifetime checks for the actual NR code.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$repo_root/build}" && pwd)
suite=${2:-memory}
case "$suite" in memory|status|benchmark|flow-benchmark) ;; *) exit 2 ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
nr="$repo_root/misc_modules/noise_reduction_logmmse/src"
sources=("$nr/arrays.cpp" "$nr/omlsa_mcra.cpp" "$nr"/omlsa_mcra/*.cpp)
if [ "$suite" = benchmark ] || [ "$suite" = flow-benchmark ]; then
    test_source=noise_reduction_benchmark
    extra=()
    if [ "$suite" = flow-benchmark ]; then test_source=logmmse_flow_benchmark;extra=(-DSDRPP_NR_DIRECT_BENCH=1);fi
    "${CXX:-c++}" -std=c++17 -O3 -Wno-deprecated-declarations -I"$repo_root/core/src" -I"$nr" \
        "${extra[@]}" "$repo_root/tests/$test_source.cpp" "${sources[@]}" \
        -L"$build" -lsdrpp_full_core -lfftw3f -lvolk -pthread -Wl,-rpath,"$build" -o "$tmp/benchmark"
    timeout --signal=TERM --kill-after=5 45s "$tmp/benchmark"
    exit 0
fi
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:quarantine_size_mb=32}"
if [ "$suite" = status ]; then
    "${CXX:-c++}" -std=c++17 -O1 -g -Wno-deprecated-declarations -fvisibility=hidden \
        -fsanitize=thread -fno-omit-frame-pointer -I"$repo_root/core/src" -I"$repo_root/core/src/imgui" -I"$nr" \
        "$repo_root/tests/noise_reduction_concurrency.cpp" "${sources[@]}" \
        -L"$build" -lsdrpp_full_core -lfftw3f -lvolk -pthread -Wl,-rpath,"$build" -o "$tmp/status"
    TSAN_OPTIONS=halt_on_error=1 timeout --signal=TERM --kill-after=5 45s "$tmp/status"
    exit 0
fi
for test in noise_reduction_signal noise_reduction_audio noise_reduction_allocations logmmse_packet_flow; do
    "${CXX:-c++}" -std=c++17 -O1 -g -Wno-deprecated-declarations -fvisibility=hidden \
        -fsanitize=address -fno-omit-frame-pointer -no-pie \
        -I"$repo_root/core/src" -I"$repo_root/core/src/imgui" -I"$nr" "$repo_root/tests/$test.cpp" "${sources[@]}" \
        -L"$build" -lsdrpp_full_core -lfftw3f -lvolk -pthread -Wl,-rpath,"$build" -o "$tmp/$test"
    timeout --signal=TERM --kill-after=5 45s "$tmp/$test"
done
