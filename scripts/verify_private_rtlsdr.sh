#!/bin/bash
# Verify the shipped ELF module, not just CMake's selected target.
set -euo pipefail
module=${1:?Usage: verify_private_rtlsdr.sh MODULE [PACKAGE_ROOT]}
package_root=${2:-}

if readelf -d "$module" | grep -E 'NEEDED.*librtlsdr' >/dev/null; then
    echo 'ERROR: RTL module still loads a shared librtlsdr' >&2
    exit 1
fi
if readelf --dyn-syms --wide "$module" | grep -E '[[:space:]]rtlsdr_[[:alnum:]_]+' >/dev/null; then
    echo 'ERROR: RTL symbols are exported or imported instead of private' >&2
    exit 1
fi
# Both strings must survive stripping: they come from reachable driver paths.
strings "$module" > "${module}.strings-check"
trap 'rm -f "${module}.strings-check"' EXIT
grep -qF 'RTL-SDR Blog V4 Lite Detected' "${module}.strings-check"
grep -qF 'leaking transfers and USB session' "${module}.strings-check"
if [ -n "$package_root" ]; then
    unwanted=$(find "$package_root" \( -name 'librtlsdr.so*' -o -name 'librtlsdr.a' \) -print)
    if [ -n "$unwanted" ]; then
        printf 'ERROR: package installs a global RTL driver:\n%s\n' "$unwanted" >&2
        exit 1
    fi
fi
echo 'OK: private static RTL driver, V4L support and patched USB teardown'
