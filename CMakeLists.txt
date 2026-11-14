cmake_minimum_required(VERSION 3.28)
project(wk VERSION 0.2.5 LANGUAGES CXX)

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

file(GLOB_RECURSE WK_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER WK_SOURCES EXCLUDE REGEX "tools/")

add_library(wk STATIC ${WK_SOURCES})
target_include_directories(wk PUBLIC include)
target_include_directories(wk PRIVATE src)
if(WIN32)
    target_link_libraries(wk PRIVATE windowscodecs)
endif()

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

    add_executable(wkview tools/wkview.cpp)
    target_link_libraries(wkview PRIVATE wk minifb)
    if(WIN32)
        target_link_libraries(wkview PRIVATE Gdi32 User32)
    elseif(APPLE)
        target_link_libraries(wkview PRIVATE "-framework Cocoa" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
    elseif(UNIX)
        target_link_libraries(wkview PRIVATE X11)
    endif()
    wk_copy_windows_runtime(wkview)
endif()

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
    wk_add_test(image_io_test tests/image_io_test.cpp)
    wk_add_test(exif_gps_test tests/exif_gps_test.cpp)
    wk_add_test(wkmeta_strict_test tests/wkmeta_strict_test.cpp)
    wk_add_test(control_field_test tests/control_field_test.cpp)
    wk_add_test(superblock_quant_test tests/superblock_quant_test.cpp)
    wk_add_test(cclm_test tests/cclm_test.cpp)
endif()

# Install the library, the command-line tools, and the public headers.
install(TARGETS wk ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(TARGETS wkenc wkdec wkmetric wkmeta-dump wkmeta-edit RUNTIME DESTINATION bin)
if(WK_BUILD_VIEWER)
    install(TARGETS wkview RUNTIME DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include)
