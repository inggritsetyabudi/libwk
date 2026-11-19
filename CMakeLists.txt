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

# Layout: src/core/ is the portable codec, src/arch/<target>/ holds
# per-target code (one folder per target, each with its own
# arch.cmake fragment), src/stb/, src/webp/ and src/avif/ are
# vendored third-party (stb image I/O, libwebp decode-only, libavif
# + dav1d decode-only). The build is split into cmake/ fragments:
# wk_sources (the single source list + reader), wk_arch (target
# detection and selection), wk_webp, wk_avif, wk_tools, wk_tests.
# This file only assembles the library from those fragments.
include(cmake/wk_sources.cmake)
include(cmake/wk_arch.cmake)
include(cmake/wk_webp.cmake)
include(cmake/wk_avif.cmake)

wk_read_sources(core WK_CORE_SOURCES)
wk_read_sources(stb WK_STB_SOURCES)

add_library(wk STATIC ${WK_CORE_SOURCES} ${WK_STB_SOURCES} ${WK_WEBP_SOURCES} ${WK_AVIF_SOURCES} ${WK_LIBYUV_SOURCES} ${WK_DAV1D_SOURCES} $<TARGET_OBJECTS:dav1d_bitdepth_8> $<TARGET_OBJECTS:dav1d_bitdepth_16> ${WK_ARCH_SOURCES})
target_include_directories(wk PUBLIC include)
target_include_directories(wk PRIVATE src src/webp src/avif/libavif/include src/avif/libavif/third_party/libyuv/include src/avif/dav1d src/avif/dav1d/include)

include(cmake/wk_tools.cmake)
include(cmake/wk_tests.cmake)

# Install the library, the command-line tools, and the public headers.
install(TARGETS wk ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(TARGETS wkenc wkdec wkmetric wkmeta-dump wkmeta-edit RUNTIME DESTINATION bin)
if(WK_BUILD_VIEWER)
    install(TARGETS wkview RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include)
