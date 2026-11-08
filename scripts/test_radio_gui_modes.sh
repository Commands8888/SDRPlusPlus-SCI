#!/bin/bash
# Run the application with a test-only synthetic IQ source under a virtual display.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$repo_root/build}" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/modules" "$tmp/config"
c++ -std=c++17 -O2 -fPIC -shared -I"$repo_root/core/src" -I"$repo_root/core/src/imgui" \
    "$repo_root/tests/radio_mode_integration.cpp" -L"$build" -lsdrpp_full_core -lglfw -lvolk -pthread \
    -Wl,-rpath,"$build" -o "$tmp/modules/mode_integration.so"
ln -s "$build/decoder_modules/radio/radio.so" "$tmp/modules/radio.so"
cat > "$tmp/config/config.json" <<EOF
{
  "modulesDirectory": "$tmp/modules",
  "resourcesDirectory": "$repo_root/root/res",
  "modules": [],
  "moduleInstances": {
    "Radio": {"module": "radio", "enabled": true},
    "Test controller": {"module": "mode_integration", "enabled": true}
  },
  "source": "Mode test IQ",
  "menuElements": [{"name": "Radio", "open": true}],
  "windowSize": {"w": 1280, "h": 720}
}
EOF
cat > "$tmp/config/radio_config.json" <<'EOF'
{"Radio": {"selectedDemodId": 0, "FM": {"highPass": true}, "AM": {"highPass": true}, "CW": {"tone": 800}, "CW-R": {"tone": 900}}}
EOF
status=0
env -u APPDIR LD_LIBRARY_PATH="$build:${LD_LIBRARY_PATH:-}" LIBGL_ALWAYS_SOFTWARE=1 \
    timeout --signal=TERM --kill-after=5 180s xvfb-run -e /dev/stderr -a -s '-screen 0 1280x720x24' \
    stdbuf -oL "$build/sdrpp-full" --root "$tmp/config" > "$tmp/gui.log" 2>&1 || status=$?
if [ "$status" -ne 0 ] || ! grep -qF 'OK: GUI RadioModule all 110 directed pairs twice' "$tmp/gui.log"; then
    tail -n 90 "$tmp/gui.log" >&2
    echo "ERROR: GUI mode test failed (status $status)" >&2
    exit 1
fi
grep -F 'OK: GUI RadioModule' "$tmp/gui.log"
# Existing CW controls must survive the full round trip through every mode.
python3 - "$tmp/config/radio_config.json" <<'PY'
import json,sys
with open(sys.argv[1]) as f: radio=json.load(f)['Radio']
assert radio['CW']['tone'] == 800
assert radio['CW-R']['tone'] == 900
assert radio['FM']['highPass'] is True
assert radio['AM']['highPass'] is True
print('OK: CW/CW-R tone and FM/AM high-pass settings preserved')
PY
