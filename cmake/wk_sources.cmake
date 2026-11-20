# cmake/wk_sources.cmake — reads cmake/wk_sources.txt, the single
# source of truth for every compilable source file. CMake, the web
# build scripts (web/build.sh, web/build.ps1) and this file all read
# the same list; no other place lists sources.
#
# Usage:
#   wk_read_sources(<section> <out-var>)
#     Sets <out-var> to the section's files as absolute paths.
#     Fails at configure time on an unknown section or a listed
#     file that does not exist.
#
# The drift check below runs at configure time: for every section,
# the files on disk (globbed) must equal the files listed. Add a
# source file in exactly one place — its section in
# cmake/wk_sources.txt — or the build refuses to configure.

# Parse the list file once into _wk_sources_<section> variables
# (paths relative to the repository root). The loop reads line by
# line with string(SUBSTRING) instead of splitting on "\n" into a
# list: comment lines may contain ';', which would split a list
# element in two.
file(READ "${CMAKE_CURRENT_LIST_DIR}/wk_sources.txt" _wk_sources_text)
string(REPLACE "\r\n" "\n" _wk_sources_text "${_wk_sources_text}")
set(_wk_sources_rest "${_wk_sources_text}")
set(_wk_sources_section "")
while(NOT _wk_sources_rest STREQUAL "")
    string(FIND "${_wk_sources_rest}" "\n" _wk_sources_nl)
    if(_wk_sources_nl EQUAL -1)
        set(_line "${_wk_sources_rest}")
        set(_wk_sources_rest "")
    else()
        string(SUBSTRING "${_wk_sources_rest}" 0 ${_wk_sources_nl} _line)
        math(EXPR _wk_sources_next "${_wk_sources_nl} + 1")
        string(SUBSTRING "${_wk_sources_rest}" ${_wk_sources_next} -1 _wk_sources_rest)
    endif()
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    if(_line MATCHES "^\\[([A-Za-z0-9_]+)\\]$")
        set(_wk_sources_section "${CMAKE_MATCH_1}")
        # Define the section even when it stays empty (a header-only
        # target), so the drift check can tell "empty" from "missing".
        set(_wk_sources_${_wk_sources_section} "")
        continue()
    endif()
    if(_wk_sources_section STREQUAL "")
        message(FATAL_ERROR "cmake/wk_sources.txt: path outside any section: ${_line}")
    endif()
    list(APPEND _wk_sources_${_wk_sources_section} "${_line}")
endwhile()

function(wk_read_sources SECTION OUT_VAR)
    if(NOT DEFINED _wk_sources_${SECTION})
        message(FATAL_ERROR "wk_read_sources: unknown section [${SECTION}] in cmake/wk_sources.txt")
    endif()
    set(_abs "")
    foreach(_rel IN LISTS _wk_sources_${SECTION})
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${_rel}")
            message(FATAL_ERROR "wk_read_sources: [${SECTION}] lists a missing file: ${_rel}")
        endif()
        list(APPEND _abs "${PROJECT_SOURCE_DIR}/${_rel}")
    endforeach()
    set(${OUT_VAR} "${_abs}" PARENT_SCOPE)
endfunction()

# _wk_sources_check(<section> <filter> <glob-pattern>...)
# filter: NONE, NO_TMPL (drop *_tmpl.c), ONLY_TMPL (keep *_tmpl.c).
function(_wk_sources_check SECTION FILTER)
    file(GLOB _on_disk RELATIVE "${PROJECT_SOURCE_DIR}" ${ARGN})
    if(FILTER STREQUAL "NO_TMPL")
        list(FILTER _on_disk EXCLUDE REGEX "_tmpl\\.c$")
    elseif(FILTER STREQUAL "ONLY_TMPL")
        list(FILTER _on_disk INCLUDE REGEX "_tmpl\\.c$")
    endif()
    list(SORT _on_disk)
    if(NOT DEFINED _wk_sources_${SECTION})
        message(FATAL_ERROR "cmake/wk_sources.txt: missing section [${SECTION}]")
    endif()
    set(_listed "${_wk_sources_${SECTION}}")
    list(SORT _listed)
    if(_on_disk STREQUAL _listed)
        return()
    endif()
    set(_only_disk "${_on_disk}")
    set(_only_listed "${_listed}")
    if(_listed)
        list(REMOVE_ITEM _only_disk ${_listed})
    endif()
    if(_on_disk)
        list(REMOVE_ITEM _only_listed ${_on_disk})
    endif()
    message(FATAL_ERROR
        "cmake/wk_sources.txt: section [${SECTION}] drifted from the tree.\n"
        "  on disk, not listed: ${_only_disk}\n"
        "  listed, not on disk: ${_only_listed}")
endfunction()

_wk_sources_check(core NONE src/core/*.cpp)
_wk_sources_check(stb NONE src/stb/*.cpp)
_wk_sources_check(webp NONE
    src/webp/src/dec/*.c src/webp/src/dsp/*.c src/webp/src/utils/*.c)
_wk_sources_check(avif NONE src/avif/libavif/src/*.c)
_wk_sources_check(libyuv NONE src/avif/libavif/third_party/libyuv/source/*.c)
_wk_sources_check(dav1d NO_TMPL src/avif/dav1d/src/*.c)
_wk_sources_check(dav1d_tmpl ONLY_TMPL src/avif/dav1d/src/*.c)
_wk_sources_check(jxl NONE
    src/jxl/lib/jxl/*.cc src/jxl/lib/jxl/render_pipeline/*.cc src/jxl/lib/jxl/modular/*.cc
    src/jxl/lib/jxl/modular/encoding/*.cc src/jxl/lib/jxl/modular/transform/*.cc
    src/jxl/lib/jxl/cms/*.cc)
_wk_sources_check(jxl_hwy NONE src/jxl/third_party/highway/hwy/*.cc)
_wk_sources_check(jxl_brotli NONE
    src/jxl/third_party/brotli/c/dec/*.c src/jxl/third_party/brotli/c/common/*.c)
_wk_sources_check(jxl_skcms NONE
    src/jxl/third_party/skcms/*.cc src/jxl/third_party/skcms/src/*.cc)
_wk_sources_check(arch_generic NONE src/arch/generic/*.cpp)
_wk_sources_check(arch_wasm NONE src/arch/wasm/*.cpp)
_wk_sources_check(arch_x86_64 NONE src/arch/x86_64/*.cpp)
_wk_sources_check(arch_aarch64 NONE src/arch/aarch64/*.cpp)
_wk_sources_check(arch_arm NONE src/arch/arm/*.cpp)
