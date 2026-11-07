cmake_minimum_required(VERSION 3.16...3.24)
project(radio)

file(GLOB_RECURSE SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(radio PRIVATE "src/")

if (OPT_BUILD_DSD)
    find_path(SDRPP_DSD_ITPP_INCLUDE_DIR itpp/itcomm.h REQUIRED)
    find_library(SDRPP_DSD_ITPP_LIBRARY NAMES itpp REQUIRED)

    set(_dsd_root "${CMAKE_CURRENT_SOURCE_DIR}/dsd")
    file(GLOB _dsd_sources "${_dsd_root}/src/dsp/*.cpp")
    file(GLOB _mbe_sources "${_dsd_root}/mbelib/*.c")

    add_library(sdrpp_full_mbe STATIC ${_mbe_sources})
    set_target_properties(sdrpp_full_mbe PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(sdrpp_full_mbe PUBLIC "${_dsd_root}/mbelib")
    if (NOT WIN32)
        target_link_libraries(sdrpp_full_mbe PUBLIC m)
    endif ()

    target_sources(radio PRIVATE ${_dsd_sources})
    target_include_directories(radio PRIVATE "${SDRPP_DSD_ITPP_INCLUDE_DIR}")
    target_compile_definitions(radio PRIVATE SDRPP_FULL_DSD=1)
    target_link_libraries(radio PRIVATE sdrpp_full_mbe "${SDRPP_DSD_ITPP_LIBRARY}")
    install(FILES "${_dsd_root}/README.md" "${_dsd_root}/LICENSE" "${_dsd_root}/mbelib/COPYRIGHT"
        DESTINATION share/doc/sdrpp-full/dsd)
endif ()
