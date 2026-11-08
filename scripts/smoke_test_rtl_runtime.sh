#!/bin/bash
set -euo pipefail
package_root=$(cd "${1:?Usage: smoke_test_rtl_runtime.sh PACKAGE_ROOT}" && pwd)
module="$package_root/usr/lib/sdrpp-full/plugins/rtl_sdr_source.so"
test -f "$module"
runtime_tmp=$(mktemp -d)
trap 'rm -rf "$runtime_tmp"' EXIT
# Start the shipped application and instantiate the shipped RTL plugin. Use
# server mode so CI needs neither an X server nor a connected receiver.
mkdir -p "$runtime_tmp/config" "$runtime_tmp/rtl-only"
ln -s "$module" "$runtime_tmp/rtl-only/rtl_sdr_source.so"
cat > "$runtime_tmp/config/config.json" <<EOF
{
  "modulesDirectory": "$runtime_tmp/rtl-only",
  "resourcesDirectory": "$package_root/usr/share/sdrpp-full",
  "modules": [],
  "moduleInstances": {
    "RTL-SDR Source": {"module": "rtl_sdr_source", "enabled": true}
  },
  "source": "RTL-SDR"
}
EOF
# A fake older driver aborts if any call escapes the private archive. Preload
# it to exercise symbol isolation as well as checking the ELF symbol table.
cat > "$runtime_tmp/poison-rtl.c" <<'EOF'
#include <stdint.h>
#include <stdlib.h>
uint32_t rtlsdr_get_device_count(void) { abort(); }
EOF
cc -shared -fPIC "$runtime_tmp/poison-rtl.c" -o "$runtime_tmp/poison-rtl.so"
status=0
env -u APPDIR LD_LIBRARY_PATH="$package_root/usr/lib" \
    LD_PRELOAD="$runtime_tmp/poison-rtl.so" LD_DEBUG=libs \
    timeout --signal=TERM --kill-after=2 5s stdbuf -oL "$package_root/usr/bin/sdrpp-full" \
    --server --addr 127.0.0.1 --port 0 --root "$runtime_tmp/config" \
    > "$runtime_tmp/startup.log" 2>&1 || status=$?
if [ "$status" -ne 124 ] ||
   ! grep -qF 'Initializing RTL-SDR Source (rtl_sdr_source)' "$runtime_tmp/startup.log" ||
   ! grep -qF 'Ready, listening on' "$runtime_tmp/startup.log" ||
   grep -qE 'find library=librtlsdr|calling init: .*librtlsdr' "$runtime_tmp/startup.log"; then
    cat "$runtime_tmp/startup.log" >&2
    echo "ERROR: packaged application/RTL startup failed (status $status)" >&2
    exit 1
fi
echo 'OK: packaged application starts with its RTL plugin; shared RTL interposition is blocked'
