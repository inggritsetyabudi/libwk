# cmake/wk_tests.cmake — the unit tests. Every test links the wk
# library defined by the root CMakeLists.

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
    wk_add_test(dither_display_test tests/dither_display_test.cpp)
    wk_add_test(decoder_raster_alloc_test tests/decoder_raster_alloc_test.cpp)

    # The source-list check: verifies cmake/wk_sources.txt against
    # the tree and the web builds. It is a test, not a diagnostic,
    # so a drift fails the suite like any other failure.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(NAME source_lists_test
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/check_source_lists.py)
    else()
        message(STATUS "Python3 not found: source_lists_test skipped")
    endif()
endif()
