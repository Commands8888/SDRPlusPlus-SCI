#!/bin/bash
# Run inside the disposable CI build container, after building the .deb.
# Extracting here tests the actual installed resource/module paths on first run.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "$1" && pwd)
package=$(realpath "$2")
test "$(id -u)" = 0
dpkg-deb -x "$package" /
tmp=$(mktemp -d)
plugin=/usr/lib/sdrpp-full/plugins/startup_integration.so
trap 'rm -f "$plugin"; rm -rf "$tmp"' EXIT
c++ -std=c++17 -O2 -fPIC -shared -I"$repo_root/core/src" -I"$repo_root/core/src/imgui" \
    "$repo_root/tests/startup_integration.cpp" -L"$build" -lsdrpp_full_core -lglfw -lvolk -pthread \
    -Wl,-rpath,"$build" -o "$plugin"
for scenario in fresh reopen resource-recovery; do
    config_root="$tmp/config"
    if [ "$scenario" = resource-recovery ]; then
        config_root="$tmp/recovery-config"
        mkdir -p "$config_root" "$tmp/resources/colormaps" "$tmp/resources/themes"
        for directory in icons fonts bandplans cty; do
            ln -s "$repo_root/root/res/$directory" "$tmp/resources/$directory"
        done
        printf '{broken' > "$tmp/resources/colormaps/bad.json"
        printf '{broken' > "$tmp/resources/themes/bad.json"
        printf '{"resourcesDirectory":"%s"}\n' "$tmp/resources" > "$config_root/config.json"
    fi
    status=0
    env -u APPDIR LD_LIBRARY_PATH="$build:${LD_LIBRARY_PATH:-}" LIBGL_ALWAYS_SOFTWARE=1 \
        timeout --signal=TERM --kill-after=5 90s xvfb-run -e /dev/stderr -a -s '-screen 0 1280x720x24' \
        stdbuf -oL "$build/sdrpp-full" --root "$config_root" > "$tmp/$scenario.log" 2>&1 || status=$?
    if [ "$status" -ne 0 ] || ! grep -qF 'OK: default startup instances drained and destroyed' "$tmp/$scenario.log"; then
        tail -n 120 "$tmp/$scenario.log" >&2
        if [ "$status" -eq 139 ] && command -v gdb >/dev/null; then
            env -u APPDIR LD_LIBRARY_PATH="$build:${LD_LIBRARY_PATH:-}" LIBGL_ALWAYS_SOFTWARE=1 \
                timeout --signal=TERM --kill-after=5 90s xvfb-run -a -s '-screen 0 1280x720x24' \
                gdb --batch --return-child-result -ex 'set print thread-events off' \
                -ex run -ex 'thread apply all bt' --args "$build/sdrpp-full" --root "$config_root" >&2 || true
        fi
        exit 1
    fi
    grep -F 'OK: default startup' "$tmp/$scenario.log"
done
python3 - "$tmp/config" <<'PY'
import json,pathlib,sys
files=list(pathlib.Path(sys.argv[1]).glob('*.json'))
assert files
for path in files:
    with path.open() as f: json.load(f)
print(f'OK: {len(files)} saved configuration files remain valid after first launch and reopen')
PY
