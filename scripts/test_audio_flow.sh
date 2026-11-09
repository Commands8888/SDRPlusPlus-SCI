#!/bin/bash
# Real callback/queue implementations, simulated devices; no sound card needed.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$repo_root/build}" && pwd)
suite=${2:-memory}
case "$suite" in memory|status) ;; *) exit 2 ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c++17 -O1 -g -fvisibility=hidden -fno-omit-frame-pointer -I"$repo_root/core/src" -I"$repo_root/core/src/imgui")
if [ "$suite" = status ]; then
    "${CXX:-c++}" "${flags[@]}" -fsanitize=thread "$repo_root/tests/audio_buffer_flow.cpp" -lvolk -pthread -o "$tmp/queue"
    TSAN_OPTIONS=halt_on_error=1 timeout --signal=TERM --kill-after=5 30s "$tmp/queue"
    "${CXX:-c++}" "${flags[@]}" -fsanitize=thread "$repo_root/tests/iq_frame_flow.cpp" -lvolk -pthread -o "$tmp/iq"
    TSAN_OPTIONS=halt_on_error=1 timeout --signal=TERM --kill-after=5 30s "$tmp/iq"
    exit 0
fi
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:quarantine_size_mb=32}"
"${CXX:-c++}" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie "$repo_root/tests/audio_buffer_flow.cpp" -lvolk -pthread -o "$tmp/queue"
timeout --signal=TERM --kill-after=5 30s "$tmp/queue"
"${CXX:-c++}" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie "$repo_root/tests/iq_frame_flow.cpp" -lvolk -pthread -o "$tmp/iq"
timeout --signal=TERM --kill-after=5 30s "$tmp/iq"
for backend in rtaudio5 rtaudio6 portaudio; do
    extra=()
    if [ "$backend" = portaudio ]; then extra=(-DSDRPP_TEST_PORTAUDIO=1); fi
    if [ "$backend" = rtaudio6 ]; then extra=(-DSDRPP_TEST_RTAUDIO_VERSION=6); fi
    "${CXX:-c++}" "${flags[@]}" -I"$repo_root/tests/fakes" -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie \
        "${extra[@]}" "$repo_root/tests/audio_sink_backend.cpp" -L"$build" -lsdrpp_full_core -lvolk -pthread -Wl,-rpath,"$build" -o "$tmp/$backend"
    timeout --signal=TERM --kill-after=5 20s "$tmp/$backend"
done
