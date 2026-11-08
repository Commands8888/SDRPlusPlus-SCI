#!/bin/bash
# Build a portable SDRPP-FULL AppImage.
# Run inside the docker_builds/appimage Dockerfile. Source tree must be
# bind-mounted at /root/SDRPlusPlus.
# Output: /root/SDRPP-FULL-v<version>-<arch>.AppImage
set -euo pipefail

SRC_DIR="${SRC_DIR:-/root/SDRPlusPlus}"
ARCH="$(uname -m)"
APP_ID="sdrpp-full"
APP_NAME="SDRPP-FULL"
APPDIR="/AppDir"
# The ci-appimage preset is authoritative for the deps setup: it pins
# SDRPP_DEPS_PRESET=appimage and SDRPP_DEPS_BUILD_DIR=deps/build-appimage
# (which the actions/cache step in build_appimage.yml relies on). This is
# just where that preset's install prefix ends up.
DEPS_PREFIX="${SRC_DIR}/deps/build-appimage/destdir/usr/local"
# VERSION_FULL includes git build info. Keep the public filename short.
: "${VERSION_FULL:?VERSION_FULL must be set (e.g. 1.2.3+45-gabc1234)}"
VERSION="${VERSION_FULL%%+*}"
OUT="/root/${APP_NAME}-v${VERSION}-${ARCH}.AppImage"

echo "=== Building AppImage for ${APP_NAME} ${VERSION_FULL} (${ARCH}) ==="
echo "Source: ${SRC_DIR}"

# Allow git operations on the bind-mounted tree.
git config --global --add safe.directory "${SRC_DIR}"

# ---------------------------------------------------------------------------
# Configure SDR++ through the root CMake project and let OPT_BUILD_DEPS drive
# the deps/ bootstrap. That keeps the dependency build aligned with the same
# module options as the main binary, so only the needed recipe closure is built.
# GLFW remains host-provided on Linux AppImage; the appimage deps preset keeps
# that exception while bundling the rest through deps/.
# ---------------------------------------------------------------------------
cd "${SRC_DIR}"
# Don't wipe deps/build-appimage unconditionally: CI restores it from the
# actions/cache step in .github/workflows/build_appimage.yml when the deps
# recipe hash hasn't changed, and autobuild.cmake's recipe-hash marker
# inside destdir/share/sdrpp-deps/ short-circuits the deps build on a
# cache hit. A stale cache (marker mismatch after a recipe edit) is
# detected by autobuild.cmake and triggers a fresh rebuild on top of the
# restored tree — overwrite-not-replace install semantics make that safe.
# The main build dir is per-run and not cached; wiping it preserves a
# from-scratch app build for repeatability.
rm -rf build

cmake --preset ci-appimage -DOPT_BUILD_DSD=ON
cd build

make -j"$(nproc)"

# ---------------------------------------------------------------------------
# Install into AppDir.
# ---------------------------------------------------------------------------
rm -rf "${APPDIR}"
make install DESTDIR="${APPDIR}"

bash "${SRC_DIR}/scripts/verify_private_rtlsdr.sh" \
    "${APPDIR}/usr/lib/sdrpp-full/plugins/rtl_sdr_source.so" "${APPDIR}"

# linuxdeploy expects the icon and desktop file at the AppDir root in addition
# to the standard usr/share/... locations.
cp "${APPDIR}/usr/share/${APP_ID}/icons/sdrpp.png" "${APPDIR}/${APP_ID}.png"

# CMake-generated desktop file points at /usr/bin/sdrpp-full. AppImages need
# Exec= to be a bare binary name and Icon= to be a basename (no extension).
DESKTOP_SRC="${APPDIR}/usr/share/applications/${APP_ID}.desktop"
DESKTOP_DST="${APPDIR}/${APP_ID}.desktop"
sed -e "s|^Exec=.*|Exec=${APP_ID}|" \
    -e "s|^Icon=.*|Icon=${APP_ID}|" \
    "${DESKTOP_SRC}" > "${DESKTOP_DST}"
cp "${DESKTOP_DST}" "${DESKTOP_SRC}"

# The window sets Wayland app_id / X11 WM_CLASS to "${APP_ID}" (see
# core/backends/glfw/backend.cpp). Desktop shells match the window to its
# launcher icon through StartupWMClass, so fail early if it ever goes missing.
if ! grep -qx "StartupWMClass=${APP_ID}" "${DESKTOP_DST}"; then
    echo "ERROR: ${DESKTOP_DST} lacks StartupWMClass=${APP_ID}" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Bundle dependencies. GPU/X11/glibc/glfw libs come from the host — bundling
# them breaks driver ABI on the user's machine.
# ---------------------------------------------------------------------------
export LD_LIBRARY_PATH="${DEPS_PREFIX}/lib:${APPDIR}/usr/lib:${LD_LIBRARY_PATH:-}"

# DSD uses ITPP from the Focal build container. Include it explicitly so the
# AppImage remains self-contained even if linuxdeploy does not scan plugins.
ITPP_LIBRARY="$(ldconfig -p | awk '$1 ~ /^libitpp[.]so[.]/ {print $NF; exit}')"
if [ -z "${ITPP_LIBRARY}" ] || [ ! -f "${ITPP_LIBRARY}" ]; then
    echo 'ERROR: libitpp runtime library is missing from the AppImage build image' >&2
    exit 1
fi

linuxdeploy \
    --appdir "${APPDIR}" \
    --desktop-file "${DESKTOP_DST}" \
    --icon-file "${APPDIR}/${APP_ID}.png" \
    --library "${ITPP_LIBRARY}" \
    --exclude-library "*libGL.so*" \
    --exclude-library "*libGLX.so*" \
    --exclude-library "*libEGL.so*" \
    --exclude-library "*libGLdispatch.so*" \
    --exclude-library "*libOpenGL.so*" \
    --exclude-library "*libdrm.so*" \
    --exclude-library "*libd3dadapter9.so*" \
    --exclude-library "*libvulkan.so*" \
    --exclude-library "*libxcb*" \
    --exclude-library "*libX11*" \
    --exclude-library "*libXext*" \
    --exclude-library "*libXrender*" \
    --exclude-library "*libnvidia*" \
    --exclude-library "*libvdpau*" \
    --exclude-library "*libLLVM*" \
    --exclude-library "*swrast*" \
    --exclude-library "*iris*" \
    --exclude-library "*radeonsi*" \
    --exclude-library "*nouveau*" \
    --exclude-library "*vmwgfx*" \
    --exclude-library "*libc.so*" \
    --exclude-library "*libglfw*"

if ! find "${APPDIR}/usr/lib" -name 'libitpp.so*' -print -quit | grep -q .; then
    echo 'ERROR: AppImage is missing the DSD ITPP runtime library' >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Replace linuxdeploy's generated AppRun with one that gives a friendly
# diagnostic when host-provided libglfw3 is missing — otherwise the user
# just sees the dynamic loader's "libglfw.so.3: cannot open shared object".
# ---------------------------------------------------------------------------
# linuxdeploy may create AppRun as a symlink to usr/bin/sdrpp-full. Remove it
# first so the here-doc below creates a new launcher instead of overwriting
# the real app binary through that symlink.
rm -f "${APPDIR}/AppRun"
cat > "${APPDIR}/AppRun" <<'APPRUN_EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"

have_glfw() {
    for ldc in /sbin/ldconfig /usr/sbin/ldconfig ldconfig; do
        if command -v "$ldc" >/dev/null 2>&1; then
            "$ldc" -p 2>/dev/null | grep -qE 'libglfw\.so\.3' && return 0
        fi
    done
    for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /lib /lib64; do
        [ -e "$d/libglfw.so.3" ] && return 0
    done
    return 1
}

if ! have_glfw; then
    cat >&2 <<'MSG'
ERROR: libglfw3 is not installed on this system.

SDRPP-FULL relies on the host's libglfw3 because GLFW links against your
system's OpenGL and X11 stack — bundling it inside the AppImage would
break GPU drivers. Install it with:

  Debian / Ubuntu / Mint:   sudo apt install libglfw3
  Fedora / RHEL / Rocky:    sudo dnf install glfw
  openSUSE:                 sudo zypper install libglfw3
  Arch / Manjaro:           sudo pacman -S glfw

Then re-run the AppImage.
MSG
    exit 1
fi

export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
export PATH="${HERE}/usr/bin:${PATH}"
exec "${HERE}/usr/bin/sdrpp-full" "$@"
APPRUN_EOF
chmod +x "${APPDIR}/AppRun"

# ---------------------------------------------------------------------------
# Pack the AppImage.
# ---------------------------------------------------------------------------
unset PKG_CONFIG_PATH
# appimagetool fetches the static type2-runtime for ${ARCH} while packing
export ARCH
appimagetool "${APPDIR}" "${OUT}"

# Verify the packed payload too, after linuxdeploy's dependency bundling.
verify_dir=$(mktemp -d)
(cd "$verify_dir" && "$OUT" --appimage-extract >/dev/null)
bash "${SRC_DIR}/scripts/verify_private_rtlsdr.sh" \
    "$verify_dir/squashfs-root/usr/lib/sdrpp-full/plugins/rtl_sdr_source.so" \
    "$verify_dir/squashfs-root"
bash "${SRC_DIR}/scripts/smoke_test_rtl_runtime.sh" "$verify_dir/squashfs-root"
rm -rf "$verify_dir"

ls -lh "${OUT}"
echo "=== AppImage ready: ${OUT} ==="
