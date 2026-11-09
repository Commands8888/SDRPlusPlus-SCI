#!/bin/bash
# Runtime regressions with real DSP/configuration/resource implementations.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$repo_root/build}" && pwd)
suite=${2:-memory}
case "$suite" in memory|status) ;; *) exit 2 ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
compile() {
    local name=$1 sanitizer=$2
    shift 2
    # The instrumented source copies must not interpose the shared core's
    # global theme/palette containers (which would initialize them twice).
    "${CXX:-c++}" -std=c++17 -O1 -g -fvisibility=hidden -fsanitize="$sanitizer" \
        -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -I"$repo_root/core/src" -I"$repo_root/core/src/imgui" \
        "$repo_root/tests/$name.cpp" "$@" -L"$build" -lsdrpp_full_core -lvolk -pthread \
        -Wl,-rpath,"$build" -o "$tmp/$name"
}
if [ "$suite" = status ]; then
    compile agc_concurrency thread
    TSAN_OPTIONS=halt_on_error=1 timeout 90s "$tmp/agc_concurrency"
    compile cw_tone_concurrency thread
    TSAN_OPTIONS=halt_on_error=1 timeout 60s "$tmp/cw_tone_concurrency"
    compile stream_stop_concurrency thread
    TSAN_OPTIONS=halt_on_error=1 timeout 45s "$tmp/stream_stop_concurrency"
    compile config_lifecycle thread "$repo_root/core/src/config.cpp"
    TSAN_OPTIONS=halt_on_error=1 timeout 45s "$tmp/config_lifecycle"
else
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:quarantine_size_mb=32}"
    compile resampler_precision address,undefined
    timeout 120s "$tmp/resampler_precision"
    compile clock_recovery_history address,undefined,float-cast-overflow -Wl,--wrap=volk_malloc
    timeout 30s "$tmp/clock_recovery_history"
    compile agc_response address,undefined
    timeout 60s "$tmp/agc_response"
    compile config_lifecycle address,undefined "$repo_root/core/src/config.cpp"
    timeout 45s "$tmp/config_lifecycle"
    compile resource_loading address,undefined "$repo_root/core/src/gui/colormaps.cpp" "$repo_root/core/src/gui/theme_manager.cpp"
    timeout 30s "$tmp/resource_loading" "$repo_root/root/res"
fi
