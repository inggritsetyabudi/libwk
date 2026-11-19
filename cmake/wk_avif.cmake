# cmake/wk_avif.cmake — vendored libavif 1.4.2 + dav1d 1.5.5
# decode-only (see src/avif/README.md). libavif sources include
# each other as "avif/...", dav1d sources as "src/...", "common/..."
# and "dav1d/..."; the include dirs in the root CMakeLists make all
# of those resolve. reformat_libyuv.c and reformat_libsharpyuv.c are
# compiled but internally stubbed (libyuv/sharpyuv off, libavif's own
# conversion). The bundled libyuv subset (third_party/libyuv) is
# compiled too: libavif's own CMake does the same when AVIF_LIBYUV
# is off — it is only used for image scaling (avifImageScale), never
# for the YUV<->RGB conversion.
wk_read_sources(avif WK_AVIF_SOURCES)
wk_read_sources(libyuv WK_LIBYUV_SOURCES)
wk_read_sources(dav1d WK_DAV1D_SOURCES)
wk_read_sources(dav1d_tmpl WK_DAV1D_TMPL_SOURCES)

# dav1d splits its sources the way meson does: 25 plain .c files
# plus 13 *_tmpl.c files that are compiled TWICE — once per
# bitdepth (8 and 16), with -DBITDEPTH=8/16. The templates use the
# BITDEPTH macro to stamp out the 8-bit and 16-bit DSP variants, so
# compiling them once (without the define) fails. The two object
# libraries below mirror meson's libdav1d_bitdepth_8/16.
foreach(_bitdepth 8 16)
    add_library(dav1d_bitdepth_${_bitdepth} OBJECT ${WK_DAV1D_TMPL_SOURCES})
    target_compile_definitions(dav1d_bitdepth_${_bitdepth} PRIVATE
        BITDEPTH=${_bitdepth} _FILE_OFFSET_BITS=64 _GNU_SOURCE)
    target_compile_options(dav1d_bitdepth_${_bitdepth} PRIVATE -ffast-math)
    target_include_directories(dav1d_bitdepth_${_bitdepth} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/avif/dav1d
        ${PROJECT_SOURCE_DIR}/src/avif/dav1d/include)
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
