cmake_minimum_required(VERSION 3.28)
project(wk VERSION 0.2.8 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_filename_component(WK_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB WK_WINDOWS_RUNTIME_DLLS
        "${WK_MINGW_BIN_DIR}/libgcc_s_*.dll"
        "${WK_MINGW_BIN_DIR}/libstdc++-6.dll"
        "${WK_MINGW_BIN_DIR}/libwinpthread-1.dll"
        "${WK_MINGW_BIN_DIR}/libssp-0.dll"
    )

    function(wk_copy_windows_runtime TARGET_NAME)
        foreach(RUNTIME_DLL IN LISTS WK_WINDOWS_RUNTIME_DLLS)
            add_custom_command(
                TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:${TARGET_NAME}>"
            )
        endforeach()
    endfunction()
else()
    function(wk_copy_windows_runtime TARGET_NAME)
    endfunction()
endif()

# Layout: src/core/ is the portable codec, src/arch/<target>/
# holds per-target code (generic fallback + wasm today; x86_64,
# aarch64 and arm when they gain real content), src/stb/, src/webp/
# and src/avif/ are vendored third-party (stb image I/O, libwebp
# decode-only, libavif + dav1d decode-only). The split is explicit
# here: core is always compiled, arch is selected by target, nothing
# relies on a recursive glob.
file(GLOB WK_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
file(GLOB WK_STB_SOURCES CONFIGURE_DEPENDS src/stb/*.cpp)
# libwebp 1.3.2 decode-only, vendored (see src/webp/README.md). The
# vendored sources include each other as "src/...", so src/webp is an
# include dir for the whole target (below).
file(GLOB WK_WEBP_SOURCES CONFIGURE_DEPENDS src/webp/src/dec/*.c src/webp/src/dsp/*.c src/webp/src/utils/*.c)
# libavif 1.4.2 + dav1d 1.5.5 decode-only, vendored (see
# src/avif/README.md). libavif sources include each other as
# "avif/...", dav1d sources as "src/...", "common/..." and
# "dav1d/..."; the include dirs below make all of those resolve.
# reformat_libyuv.c and reformat_libsharpyuv.c are compiled but
# internally stubbed (libyuv/sharpyuv off, libavif's own conversion).
# The bundled libyuv subset (third_party/libyuv) is compiled too:
# libavif's own CMake does the same when AVIF_LIBYUV is off — it is
# only used for image scaling (avifImageScale), never for the
# YUV<->RGB conversion.
file(GLOB WK_AVIF_SOURCES CONFIGURE_DEPENDS src/avif/libavif/src/*.c)
file(GLOB WK_LIBYUV_SOURCES CONFIGURE_DEPENDS src/avif/libavif/third_party/libyuv/source/*.c)
# dav1d splits its sources the way meson does: 25 plain .c files
# plus 13 *_tmpl.c files that are compiled TWICE — once per
# bitdepth (8 and 16), with -DBITDEPTH=8/16. The templates use the
# BITDEPTH macro to stamp out the 8-bit and 16-bit DSP variants, so
# compiling them once (without the define) fails. The two object
# libraries below mirror meson's libdav1d_bitdepth_8/16.
file(GLOB WK_DAV1D_SOURCES CONFIGURE_DEPENDS
    src/avif/dav1d/src/cdf.c src/avif/dav1d/src/cpu.c src/avif/dav1d/src/ctx.c
    src/avif/dav1d/src/data.c src/avif/dav1d/src/decode.c src/avif/dav1d/src/dequant_tables.c
    src/avif/dav1d/src/getbits.c src/avif/dav1d/src/intra_edge.c src/avif/dav1d/src/itx_1d.c
    src/avif/dav1d/src/lf_mask.c src/avif/dav1d/src/lib.c src/avif/dav1d/src/log.c
    src/avif/dav1d/src/mem.c src/avif/dav1d/src/msac.c src/avif/dav1d/src/obu.c
    src/avif/dav1d/src/pal.c src/avif/dav1d/src/picture.c src/avif/dav1d/src/qm.c
    src/avif/dav1d/src/ref.c src/avif/dav1d/src/refmvs.c src/avif/dav1d/src/scan.c
    src/avif/dav1d/src/tables.c src/avif/dav1d/src/thread_task.c src/avif/dav1d/src/warpmv.c
    src/avif/dav1d/src/wedge.c)
file(GLOB WK_DAV1D_TMPL_SOURCES CONFIGURE_DEPENDS
    src/avif/dav1d/src/cdef_apply_tmpl.c src/avif/dav1d/src/cdef_tmpl.c
    src/avif/dav1d/src/fg_apply_tmpl.c src/avif/dav1d/src/filmgrain_tmpl.c
    src/avif/dav1d/src/ipred_prepare_tmpl.c src/avif/dav1d/src/ipred_tmpl.c
    src/avif/dav1d/src/itx_tmpl.c src/avif/dav1d/src/lf_apply_tmpl.c
    src/avif/dav1d/src/loopfilter_tmpl.c src/avif/dav1d/src/looprestoration_tmpl.c
    src/avif/dav1d/src/lr_apply_tmpl.c src/avif/dav1d/src/mc_tmpl.c
    src/avif/dav1d/src/recon_tmpl.c)
foreach(_bitdepth 8 16)
    add_library(dav1d_bitdepth_${_bitdepth} OBJECT ${WK_DAV1D_TMPL_SOURCES})
    target_compile_definitions(dav1d_bitdepth_${_bitdepth} PRIVATE
        BITDEPTH=${_bitdepth} _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_compile_options(dav1d_bitdepth_${_bitdepth} PRIVATE -ffast-math)
    target_include_directories(dav1d_bitdepth_${_bitdepth} PRIVATE
        src/avif/dav1d src/avif/dav1d/include)
endforeach()
# The dav1d decoder registers in libavif only when AVIF_CODEC_DAV1D
# is defined; the dav1d sources need the same defines as the meson
# build (_FILE_OFFSET_BITS=64, _GNU_SOURCE). libavif needs C11
# (io.c uses static_assert); the repo default stays C99, so the
# standard is raised for the libavif sources only — as a compile
# option, which lands after the target-wide -std=gnu99 and wins.
set_source_files_properties(${WK_AVIF_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS AVIF_CODEC_DAV1D
    COMPILE_OPTIONS "-std=c11")
set_source_files_properties(${WK_DAV1D_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "_FILE_OFFSET_BITS=64;_GNU_SOURCE"
    COMPILE_OPTIONS "-ffast-math")

if(EMSCRIPTEN)
    set(WK_ARCH_DIR wasm)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(WK_ARCH_DIR x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(WK_ARCH_DIR aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    set(WK_ARCH_DIR arm)
else()
    set(WK_ARCH_DIR generic)
endif()
message(STATUS "wk arch: ${WK_ARCH_DIR} (processor: ${CMAKE_SYSTEM_PROCESSOR})")

file(GLOB WK_ARCH_SOURCES CONFIGURE_DEPENDS src/arch/generic/*.cpp)
if(NOT WK_ARCH_DIR STREQUAL "generic")
    file(GLOB WK_ARCH_TARGET_SOURCES CONFIGURE_DEPENDS src/arch/${WK_ARCH_DIR}/*.cpp)
    list(APPEND WK_ARCH_SOURCES ${WK_ARCH_TARGET_SOURCES})
endif()

add_library(wk STATIC ${WK_CORE_SOURCES} ${WK_STB_SOURCES} ${WK_WEBP_SOURCES} ${WK_AVIF_SOURCES} ${WK_LIBYUV_SOURCES} ${WK_DAV1D_SOURCES} $<TARGET_OBJECTS:dav1d_bitdepth_8> $<TARGET_OBJECTS:dav1d_bitdepth_16> ${WK_ARCH_SOURCES})
target_include_directories(wk PUBLIC include)
target_include_directories(wk PRIVATE src src/webp src/avif/libavif/include src/avif/libavif/third_party/libyuv/include src/avif/dav1d src/avif/dav1d/include)

add_executable(wkenc tools/wkenc.cpp)
target_link_libraries(wkenc PRIVATE wk)
wk_copy_windows_runtime(wkenc)

add_executable(wkdec tools/wkdec.cpp)
target_link_libraries(wkdec PRIVATE wk)
wk_copy_windows_runtime(wkdec)

add_executable(wkmetric tools/wkmetric.cpp)
target_link_libraries(wkmetric PRIVATE wk)
wk_copy_windows_runtime(wkmetric)

add_executable(wkmeta-dump tools/wkmeta_dump.cpp)
target_link_libraries(wkmeta-dump PRIVATE wk)
wk_copy_windows_runtime(wkmeta-dump)

add_executable(wkmeta-edit tools/wkmeta_edit.cpp)
target_link_libraries(wkmeta-edit PRIVATE wk)
wk_copy_windows_runtime(wkmeta-edit)

option(WK_BUILD_VIEWER "Build graphical viewer" ON)
if(WK_BUILD_VIEWER)
    include(FetchContent)
    FetchContent_Declare(
        minifb
        GIT_REPOSITORY https://github.com/emoon/minifb.git
        # v0.10.1, pinned to its commit so the build does not move
        # when the tag does.
        GIT_TAG 51f8004058a99fe024328b7f0848a6e353bfac86
    )
    if(WIN32)
        set(MINIFB_USE_OPENGL_API OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(minifb)
endif()

# wkview is always built: with WK_BUILD_VIEWER it opens a window when a
# display exists (and falls back to the terminal when it cannot); without
# it the binary is terminal-only (--cli), which is what Termux needs.
add_executable(wkview tools/wkview.cpp)
target_link_libraries(wkview PRIVATE wk)
if(WK_BUILD_VIEWER)
    target_compile_definitions(wkview PRIVATE WK_VIEWER_GUI)
    target_link_libraries(wkview PRIVATE minifb)
    if(WIN32)
        target_link_libraries(wkview PRIVATE Gdi32 User32)
    elseif(APPLE)
        target_link_libraries(wkview PRIVATE "-framework Cocoa" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
    elseif(UNIX)
        target_link_libraries(wkview PRIVATE X11)
    endif()
endif()
wk_copy_windows_runtime(wkview)

option(WK_BUILD_TESTS "Build unit tests" ON)
if(WK_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        # v1.14.0, pinned to its commit so the build does not move
        # when the tag does.
        GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    function(wk_add_test TEST_NAME TEST_SOURCE)
        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE wk GTest::gtest_main)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        wk_copy_windows_runtime(${TEST_NAME})
    endfunction()

    wk_add_test(rans_test tests/rans_test.cpp)
    wk_add_test(roundtrip_test tests/roundtrip_test.cpp)
    wk_add_test(wkmeta_test tests/wkmeta_test.cpp)
    wk_add_test(geo_roundtrip_test tests/geo_roundtrip_test.cpp)
    wk_add_test(container_test tests/container_test.cpp)
    wk_add_test(dct_test tests/dct_test.cpp)
    wk_add_test(predict_test tests/predict_test.cpp)
    wk_add_test(coeff_sign_stream_test tests/coeff_sign_stream_test.cpp)
    wk_add_test(coeff_presence_stream_test tests/coeff_presence_stream_test.cpp)
    wk_add_test(coeff_table_bank_stream_test tests/coeff_table_bank_stream_test.cpp)
    wk_add_test(lossy_coeff_stream_test tests/lossy_coeff_stream_test.cpp)
    wk_add_test(coeff_span_stream_test tests/coeff_span_stream_test.cpp)
    wk_add_test(coeff_table_stream_test tests/coeff_table_stream_test.cpp)
    wk_add_test(mode_stream_test tests/mode_stream_test.cpp)
    wk_add_test(mode_span_rans_test tests/mode_span_rans_test.cpp)
    wk_add_test(quantize_test tests/quantize_test.cpp)
    wk_add_test(metrics_test tests/metrics_test.cpp)
    wk_add_test(c_api_test tests/c_api_test.cpp)
    wk_add_test(wk_hpp_test tests/wk_hpp_test.cpp)
    wk_add_test(threading_test tests/threading_test.cpp)
    wk_add_test(arch_test tests/arch_test.cpp)
    wk_add_test(image_io_test tests/image_io_test.cpp)
    wk_add_test(exif_gps_test tests/exif_gps_test.cpp)
    wk_add_test(wkmeta_strict_test tests/wkmeta_strict_test.cpp)
    wk_add_test(control_field_test tests/control_field_test.cpp)
    wk_add_test(skin_protection_test tests/skin_protection_test.cpp)
    wk_add_test(skin_fp_test tests/skin_fp_test.cpp)
    wk_add_test(superblock_quant_test tests/superblock_quant_test.cpp)
    wk_add_test(cclm_test tests/cclm_test.cpp)
    wk_add_test(adaptive_chroma_test tests/adaptive_chroma_test.cpp)
    wk_add_test(hdr_test tests/hdr_test.cpp)
    wk_add_test(tonemap_test tests/tonemap_test.cpp)
    wk_add_test(tool_log_test tests/tool_log_test.cpp)
    wk_add_test(tile_util_test tests/tile_util_test.cpp)
endif()

# Install the library, the command-line tools, and the public headers.
install(TARGETS wk ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(TARGETS wkenc wkdec wkmetric wkmeta-dump wkmeta-edit RUNTIME DESTINATION bin)
if(WK_BUILD_VIEWER)
    install(TARGETS wkview RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include)
