#
# librtlsdr 2.0.3 from Osmocom, including RTL-SDR Blog V4L support.
# Vojtech Bubnik's USB teardown fix is applied locally. Linux packages embed
# only the static library in rtl_sdr_source; no shared driver is installed.
#
sdrpp_dep_is_system(libusb _rtlsdr_system_usb)
set(_rtlsdr_usb_args "")
if (NOT _rtlsdr_system_usb)
    # AppImage supplies explicit libusb paths through DEP_CMAKE_OPTS. The
    # upstream pkg-config probe would overwrite those with host paths (or
    # fail when the host has no libusb-dev), losing the bundled dependency.
    list(APPEND _rtlsdr_usb_args -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON)
endif ()

add_cmake_project(librtlsdr
    GIT_REPOSITORY https://github.com/osmocom/rtl-sdr
    # v2.0.3 (2026-08-11); update intentionally after testing.
    GIT_TAG        797f8143266d983c56d8f35d2d442527529dd8a5
    GIT_SHALLOW    OFF
    PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DSRC=<SOURCE_DIR>
                       -P ${CMAKE_CURRENT_LIST_DIR}/patch_librtlsdr.cmake
    INSTALL_COMMAND
        ${CMAKE_COMMAND} --build . --target install --config ${CMAKE_BUILD_TYPE}
        COMMAND ${CMAKE_COMMAND}
            -DROOT=${SDRPP_DEPS_INSTALL_PREFIX}
            -P ${CMAKE_CURRENT_LIST_DIR}/fix_librtlsdr_config.cmake
    CMAKE_ARGS
        ${_rtlsdr_usb_args}
        -DBUILD_UTILITIES=OFF
        -DINSTALL_UDEV_RULES=OFF
        -DDETACH_KERNEL_DRIVER=ON
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

set(DEP_librtlsdr_DEPENDS libusb)

sdrpp_validate_dep(librtlsdr
    PACKAGE_NAME     rtlsdr
    TARGET           rtlsdr::rtlsdr
    STATIC_TARGET    rtlsdr::rtlsdr_static
    SHARED_TARGET    rtlsdr::rtlsdr
    LIB_NAMES        rtlsdr
    # Upstream's static target keeps the `_static` suffix on Windows but
    # overrides OUTPUT_NAME back to `rtlsdr` on UNIX, producing `librtlsdr.a`
    # alongside `librtlsdr.so*`. List both basenames so find_library matches
    # either install layout.
    STATIC_LIB_NAMES rtlsdr_static rtlsdr
    SHARED_LIB_NAMES rtlsdr
    DLL_NAMES        rtlsdr.dll
    HEADER           rtl-sdr.h
    CONFIG_SUBDIR    rtlsdr
    REQUIRES_CONFIG)
