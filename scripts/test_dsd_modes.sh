#!/bin/bash
# Run after a Linux build with OPT_BUILD_DSD=ON; needs the compiler, libitpp
# and Volk development packages used by that build. No SDR/audio device needed.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$repo_root/build}" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
dsd="$repo_root/decoder_modules/radio/dsd"
radio="$build/decoder_modules/radio/radio.so"
test -f "$radio"

compile() {
    local name=$1 sanitizer=$2
    c++ -std=c++17 -O1 -g -DSDRPP_FULL_DSD=1 \
        -fsanitize="$sanitizer" -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -I"$repo_root/core/src" -I"$repo_root/core/src/imgui" \
        -I"$repo_root/decoder_modules/radio/src" -I"$dsd/src/dsp" -I"$dsd/mbelib" \
        "$repo_root/tests/$name.cpp" "$dsd/src/dsp/dsd_demod.cpp" \
        "$radio" -L"$build" -lsdrpp_full_core -litpp -lvolk -pthread \
        -Wl,-rpath,"$build:$build/decoder_modules/radio" -o "$tmp/$name"
}

compile dsd_input_buffer address,undefined
for packet in 120 240 241; do
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:quarantine_size_mb=32}" \
        timeout 15s "$tmp/dsd_input_buffer" "$packet"
done
compile dsd_mode_switch_stress address,undefined
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:quarantine_size_mb=32}" \
    timeout 90s "$tmp/dsd_mode_switch_stress"
compile dsd_status_concurrency thread
TSAN_OPTIONS=halt_on_error=1 timeout 30s "$tmp/dsd_status_concurrency"
