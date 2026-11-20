# cmake/wk_jxl.cmake — vendored libjxl 0.13.0 decode-only (see
# src/jxl/README.md). The vendored sources include each other as
# "lib/jxl/...", "jxl/...", "hwy/...", "brotli/..." and "skcms.h";
# the include dirs in the root CMakeLists make all of those resolve.
# Native only: JXL is not part of the web build (no Emscripten port
# for libjxl), so this fragment is not referenced by web/build.sh or
# web/build.ps1, and the jxl sections are not in WEB_SECTIONS in
# cmake/check_source_lists.py.
wk_read_sources(jxl WK_JXL_SOURCES)
wk_read_sources(jxl_hwy WK_JXL_HWY_SOURCES)
wk_read_sources(jxl_brotli WK_JXL_BROTLI_SOURCES)
wk_read_sources(jxl_skcms WK_JXL_SKCMS_SOURCES)

# Upstream builds the decoder as C++17 with -fno-rtti
# -fno-exceptions (the repo default is C++23); the standard is set
# per-source, as a compile option, which lands after the target-wide
# -std=c++23 and wins — the same pattern wk_avif.cmake uses for
# libavif's -std=c11. The -fno-rtti / -fno-exceptions pair is
# deliberately NOT mirrored: with -fno-rtti the vendored TUs do not
# emit typeinfo for libjxl's polymorphic classes (ColorEncoding,
# Customxy, ...), while the repo's own jxl_decode.cpp (RTTI on)
# needs it for the ICC fallback — the Debug/ASan build (-O0, no
# devirtualization) then fails to link with undefined references
# to `typeinfo for jxl::ColorEncoding`. Release (-O3) linked only
# because the references were devirtualized away. Compiling the
# vendored sources with the repo defaults (RTTI/exceptions on, as
# the AVIF and WebP vendors do) emits the typeinfo normally and
# links at every optimization level.
# - JPEGXL_ENABLE_TRANSCODE_JPEG=0: the 4 JPEG-reconstruction files
#   are not vendored; every use in decode.cc is #if-guarded.
# - JPEGXL_ENABLE_BOXES=1: box container support (the 16-bit
#   container fixture needs it).
# - JPEGXL_ENABLE_SKCMS=1: the ICC fallback path uses jxl_cms.cc.
# - JXL_STATIC_DEFINE: static linking (JXL_EXPORT empty).
# - JXL_INTERNAL_LIBRARY_BUILD: as in the research build (a no-op
#   in 0.13: both branches of jxl_export.h give the same result).
# - HWY_STATIC_DEFINE / TOOLCHAIN_MISS_ASM_HWCAP_H: highway, as in
#   the research build.
# - JXL_CMS_STATIC_DEFINE / SKCMS_DISABLE_HSW / SKCMS_DISABLE_SKX:
#   the CMS/skcms sources, as in the research build's jxl_cms
#   target. jxl_cms.cc lives in section [jxl] (its directory), so
#   the CMS-only define is set on that one file below.
# Deliberately NOT mirrored from the research build:
# HWY_DISABLED_TARGETS (upstream default instead — runtime dispatch
# picks the fastest target). Note: the lossy float pipeline is NOT
# bit-identical across SIMD widths — measured 20 Nov 2026:
# repo build (AVX512 dispatched) vs research build (AVX3 disabled,
# AVX2 path) differ on 238/1,179,648 samples (chart) and
# 113/1,179,648 (memorial), max diff 1 LSB 10-bit; the lossless
# integer path IS bit-identical. Upstream default kept for speed.
set_source_files_properties(${WK_JXL_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS
        "JPEGXL_ENABLE_TRANSCODE_JPEG=0;JPEGXL_ENABLE_BOXES=1;JPEGXL_ENABLE_SKCMS=1;JXL_STATIC_DEFINE;JXL_INTERNAL_LIBRARY_BUILD"
    COMPILE_OPTIONS "-std=c++17")
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/jxl/lib/jxl/cms/jxl_cms.cc PROPERTIES
    COMPILE_DEFINITIONS
        "JPEGXL_ENABLE_TRANSCODE_JPEG=0;JPEGXL_ENABLE_BOXES=1;JPEGXL_ENABLE_SKCMS=1;JXL_STATIC_DEFINE;JXL_INTERNAL_LIBRARY_BUILD;HWY_STATIC_DEFINE;JXL_CMS_STATIC_DEFINE;SKCMS_DISABLE_HSW;SKCMS_DISABLE_SKX;TOOLCHAIN_MISS_ASM_HWCAP_H"
    COMPILE_OPTIONS "-std=c++17")
set_source_files_properties(${WK_JXL_HWY_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "HWY_STATIC_DEFINE;TOOLCHAIN_MISS_ASM_HWCAP_H;JXL_STATIC_DEFINE"
    COMPILE_OPTIONS "-std=c++17")
set_source_files_properties(${WK_JXL_SKCMS_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS
        "HWY_STATIC_DEFINE;JXL_CMS_STATIC_DEFINE;JXL_STATIC_DEFINE;SKCMS_DISABLE_HSW;SKCMS_DISABLE_SKX;TOOLCHAIN_MISS_ASM_HWCAP_H;JPEGXL_ENABLE_SKCMS=1"
    COMPILE_OPTIONS "-std=c++17")
# Brotli needs no special defines: the research build compiled it
# with default C flags. It only needs its include dir (set on the
# wk target in the root CMakeLists).
