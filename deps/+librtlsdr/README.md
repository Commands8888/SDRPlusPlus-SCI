# Private RTL-SDR driver

SDRPP-FULL v1.7.6 embeds Osmocom rtl-sdr **2.0.3**, commit
`797f8143266d983c56d8f35d2d442527529dd8a5`, in `rtl_sdr_source.so`.
This version includes RTL-SDR Blog V4L (R828S) support and retains the
existing V3/V4 and other supported tuner paths.

Source: https://github.com/osmocom/rtl-sdr/tree/797f8143266d983c56d8f35d2d442527529dd8a5

The USB teardown correction is by **Vojtech Bubnik**, from SDRIAK:
https://github.com/bubnikv/sdriak/blob/master/deps/%2Blibrtlsdr/0002-fix-use-after-free-in-async-teardown.patch

It prevents resubmission after cancellation, cancels outstanding transfers,
drains callbacks for up to 500 ms and avoids freeing memory still owned by
libusb on an unrecoverable USB session. The timeout applies to stream
teardown, not application startup. An unreapable session can retain memory
until process exit; a warning is printed in that exceptional path.

The local build patch honors `BUILD_SHARED_LIBS=OFF` and
`BUILD_UTILITIES=OFF`. Only the PIC archive is installed in the temporary
build prefix; no librtlsdr shared library, utilities or udev rules are
installed in the user's OS. Archive symbols are hidden in the RTL plugin
to prevent interposition by another librtlsdr loaded in the same process.
The separate libusb dependency keeps its existing system/AppImage policy.

Linux builds require the patched archive and reject a system/shared RTL
fallback. Use `cmake --preset ci-linux-deb -DOPT_BUILD_DSD=ON` or
`cmake --preset ci-appimage -DOPT_BUILD_DSD=ON`.

Package verification checks ELF dependencies, symbol visibility and the
presence of both the V4L and teardown code paths. The async regression test
uses simulated libusb failures; reception on physical V3/V4/V4L hardware
must also be checked by users with those devices.

The upstream source is GPL-2.0-or-later; see `COPYING.rtl-sdr` and upstream
file headers. Both local patches are included in this directory.
