#!/bin/bash
# Run in the distro build container; all package dependencies are present.
set -euo pipefail
package=${1:?Usage: smoke_test_deb.sh PACKAGE}
repo_root=$(cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dpkg-deb --extract "$package" "$stage"
module=$(find "$stage" -name rtl_sdr_source.so -print -quit)
test -n "$module"
bash "$repo_root/scripts/verify_private_rtlsdr.sh" "$module" "$stage"
if dpkg-deb -f "$package" Depends | grep -q 'librtlsdr'; then
    echo 'ERROR: Debian package still depends on system librtlsdr' >&2
    exit 1
fi
test -s "$stage/usr/share/sdrpp-full/bandplans/Geral.json"
test -s "$stage/usr/share/sdrpp-full/icons/sdrpp.png"
echo 'OK: Debian package contents and runtime dependency isolation'

bash "$repo_root/scripts/smoke_test_rtl_runtime.sh" "$stage"
