cmake_minimum_required(VERSION 3.16...3.24)
project(rtl_sdr_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

sdrpp_link_dep(rtl_sdr_source librtlsdr
    PACKAGE_NAME   rtlsdr
    TARGETS        rtlsdr::rtlsdr rtlsdr
    STATIC_TARGETS rtlsdr::rtlsdr_static
    SHARED_TARGETS rtlsdr::rtlsdr)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # A system/shared fallback would silently lose V4L support and the USB
    # teardown fix. Require the patched archive, even with host -dev installed.
    sdrpp_resolve_dep_policy(librtlsdr _rtl_policy)
    if (TARGET rtlsdr::rtlsdr_static)
        get_target_property(_rtl_version rtlsdr::rtlsdr_static SDRPP_RTLSDR_PRIVATE_VERSION)
        get_target_property(_rtl_patched rtlsdr::rtlsdr_static SDRPP_RTLSDR_PRIVATE_PATCHED)
    endif ()
    if (NOT _rtl_policy_SOURCE STREQUAL "bundled" OR
        NOT _rtl_policy_LINKAGE STREQUAL "static" OR
        NOT TARGET rtlsdr::rtlsdr_static OR
        NOT _rtl_patched OR
        NOT _rtl_version VERSION_EQUAL "2.0.3")
        message(FATAL_ERROR "RTL-SDR requires the private patched 2.0.3 archive. Configure with --preset ci-linux-deb or ci-appimage.")
    endif ()
    # Hide archive symbols so dlopen/LD_PRELOAD cannot interpose another
    # librtlsdr in this process, or make other plugins bind to our copy.
    target_link_options(rtl_sdr_source PRIVATE "LINKER:--exclude-libs,librtlsdr.a")
    install(FILES
        "${CMAKE_SOURCE_DIR}/deps/+librtlsdr/README.md"
        "${CMAKE_SOURCE_DIR}/deps/+librtlsdr/COPYING.rtl-sdr"
        DESTINATION share/doc/sdrpp-full/rtl-sdr)
endif ()
# librtlsdr's Config.cmake / .pc declares libusb-1.0 transitively, so a
# separate libusb link is normally not required. Keep an explicit fallback
# for environments where the transitive declaration is absent.
sdrpp_link_dep(rtl_sdr_source libusb PC_NAME libusb-1.0)
