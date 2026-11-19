# Copyright 2026 Inggrit Setya Budi
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

$exported_functions = @(
    '_wk_wasm_alloc',
    '_wk_wasm_free',
    '_wk_wasm_decode',
    '_wk_wasm_encode',
    '_wk_wasm_version',
    '_wk_wasm_get_info',
    '_malloc',
    '_free'
) -join ','

# Vendored libwebp 1.3.2 decode-only C sources (see src/webp/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++ and
# libwebp is C. Listed explicitly, keep in sync with src/webp/.
# The objects are linked by em++ below, then removed.
$webpSources = @(
  '../src/webp/src/dec/alpha_dec.c',
  '../src/webp/src/dec/buffer_dec.c',
  '../src/webp/src/dec/frame_dec.c',
  '../src/webp/src/dec/io_dec.c',
  '../src/webp/src/dec/quant_dec.c',
  '../src/webp/src/dec/tree_dec.c',
  '../src/webp/src/dec/vp8_dec.c',
  '../src/webp/src/dec/vp8l_dec.c',
  '../src/webp/src/dec/webp_dec.c',
  '../src/webp/src/dsp/alpha_processing.c',
  '../src/webp/src/dsp/alpha_processing_neon.c',
  '../src/webp/src/dsp/alpha_processing_sse2.c',
  '../src/webp/src/dsp/alpha_processing_sse41.c',
  '../src/webp/src/dsp/cpu.c',
  '../src/webp/src/dsp/dec.c',
  '../src/webp/src/dsp/dec_clip_tables.c',
  '../src/webp/src/dsp/dec_neon.c',
  '../src/webp/src/dsp/dec_sse2.c',
  '../src/webp/src/dsp/dec_sse41.c',
  '../src/webp/src/dsp/filters.c',
  '../src/webp/src/dsp/filters_neon.c',
  '../src/webp/src/dsp/filters_sse2.c',
  '../src/webp/src/dsp/lossless.c',
  '../src/webp/src/dsp/lossless_neon.c',
  '../src/webp/src/dsp/lossless_sse2.c',
  '../src/webp/src/dsp/lossless_sse41.c',
  '../src/webp/src/dsp/rescaler.c',
  '../src/webp/src/dsp/rescaler_neon.c',
  '../src/webp/src/dsp/rescaler_sse2.c',
  '../src/webp/src/dsp/upsampling.c',
  '../src/webp/src/dsp/upsampling_neon.c',
  '../src/webp/src/dsp/upsampling_sse2.c',
  '../src/webp/src/dsp/upsampling_sse41.c',
  '../src/webp/src/dsp/yuv.c',
  '../src/webp/src/dsp/yuv_neon.c',
  '../src/webp/src/dsp/yuv_sse2.c',
  '../src/webp/src/dsp/yuv_sse41.c',
  '../src/webp/src/utils/bit_reader_utils.c',
  '../src/webp/src/utils/color_cache_utils.c',
  '../src/webp/src/utils/huffman_utils.c',
  '../src/webp/src/utils/quant_levels_dec_utils.c',
  '../src/webp/src/utils/random_utils.c',
  '../src/webp/src/utils/rescaler_utils.c',
  '../src/webp/src/utils/thread_utils.c',
  '../src/webp/src/utils/utils.c'
)
$webpObjDir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("wk-webp-" + [guid]::NewGuid().ToString('N'))) -Force
$webpObjs = @()
foreach ($src in $webpSources) {
  $obj = Join-Path $webpObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '.o')
  emcc -I../src/webp -O3 -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
  $webpObjs += $obj
}

# Vendored libavif 1.4.2 decode-only C sources (see src/avif/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++
# and libavif is C. AVIF_CODEC_DAV1D registers the dav1d decoder.
# Listed explicitly, keep in sync with src/avif/libavif/src/.
$avifSources = @(
  '../src/avif/libavif/src/alpha.c',,
  '../src/avif/libavif/src/avif.c',,
  '../src/avif/libavif/src/codec_dav1d.c',,
  '../src/avif/libavif/src/colr.c',,
  '../src/avif/libavif/src/colrconvert.c',,
  '../src/avif/libavif/src/diag.c',,
  '../src/avif/libavif/src/exif.c',,
  '../src/avif/libavif/src/gainmap.c',,
  '../src/avif/libavif/src/io.c',,
  '../src/avif/libavif/src/mem.c',,
  '../src/avif/libavif/src/obu.c',,
  '../src/avif/libavif/src/properties.c',,
  '../src/avif/libavif/src/rawdata.c',,
  '../src/avif/libavif/src/read.c',,
  '../src/avif/libavif/src/reformat.c',,
  '../src/avif/libavif/src/reformat_libsharpyuv.c',,
  '../src/avif/libavif/src/reformat_libyuv.c',,
  '../src/avif/libavif/src/sampletransform.c',,
  '../src/avif/libavif/src/scale.c',,
  '../src/avif/libavif/src/stream.c',,
  '../src/avif/libavif/src/utils.c',,
  '../src/avif/libavif/src/write.c',
)
$avifObjDir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("wk-avif-" + [guid]::NewGuid().ToString('N'))) -Force
$avifObjs = @()
foreach ($src in $avifSources) {
  $obj = Join-Path $avifObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '.o')
  emcc -I../src/avif/libavif/include -I../src/avif/libavif/third_party/libyuv/include -I../src/avif/dav1d/include -DAVIF_CODEC_DAV1D -std=c11 -O3 -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
  $avifObjs += $obj
}

# Vendored dav1d 1.5.5 decode-only C sources (see src/avif/README.md):
# C-only build (no asm; nasm is unavailable), same defines as the
# meson build. The vendored config.h carries a __EMSCRIPTEN__
# override that turns the x86 arch flags off for wasm.
# The 13 *_tmpl.c files are compiled TWICE, once per bitdepth
# (-DBITDEPTH=8 and -DBITDEPTH=16) — meson does the same
# (libdav1d_bitdepth_8/16). Listed explicitly, keep in sync with
# src/avif/dav1d/src/.
$dav1dSources = @(
  '../src/avif/dav1d/src/cdf.c',,
  '../src/avif/dav1d/src/cpu.c',,
  '../src/avif/dav1d/src/ctx.c',,
  '../src/avif/dav1d/src/data.c',,
  '../src/avif/dav1d/src/decode.c',,
  '../src/avif/dav1d/src/dequant_tables.c',,
  '../src/avif/dav1d/src/getbits.c',,
  '../src/avif/dav1d/src/intra_edge.c',,
  '../src/avif/dav1d/src/itx_1d.c',,
  '../src/avif/dav1d/src/lf_mask.c',,
  '../src/avif/dav1d/src/lib.c',,
  '../src/avif/dav1d/src/log.c',,
  '../src/avif/dav1d/src/mem.c',,
  '../src/avif/dav1d/src/msac.c',,
  '../src/avif/dav1d/src/obu.c',,
  '../src/avif/dav1d/src/pal.c',,
  '../src/avif/dav1d/src/picture.c',,
  '../src/avif/dav1d/src/qm.c',,
  '../src/avif/dav1d/src/ref.c',,
  '../src/avif/dav1d/src/refmvs.c',,
  '../src/avif/dav1d/src/scan.c',,
  '../src/avif/dav1d/src/tables.c',,
  '../src/avif/dav1d/src/thread_task.c',,
  '../src/avif/dav1d/src/warpmv.c',,
  '../src/avif/dav1d/src/wedge.c',
)
$dav1dObjDir = New-Item -ItemType Directory -Path (Join-Path ([System.IO.Path]::GetTempPath()) ("wk-dav1d-" + [guid]::NewGuid().ToString('N'))) -Force
$dav1dObjs = @()
foreach ($src in $dav1dSources) {
  $obj = Join-Path $dav1dObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '.o')
  emcc -I../src/avif/dav1d -I../src/avif/dav1d/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -ffast-math -O3 -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
  $dav1dObjs += $obj
}
$dav1dTmplSources = @(
  '../src/avif/dav1d/src/cdef_apply_tmpl.c',,
  '../src/avif/dav1d/src/cdef_tmpl.c',,
  '../src/avif/dav1d/src/fg_apply_tmpl.c',,
  '../src/avif/dav1d/src/filmgrain_tmpl.c',,
  '../src/avif/dav1d/src/ipred_prepare_tmpl.c',,
  '../src/avif/dav1d/src/ipred_tmpl.c',,
  '../src/avif/dav1d/src/itx_tmpl.c',,
  '../src/avif/dav1d/src/lf_apply_tmpl.c',,
  '../src/avif/dav1d/src/loopfilter_tmpl.c',,
  '../src/avif/dav1d/src/looprestoration_tmpl.c',,
  '../src/avif/dav1d/src/lr_apply_tmpl.c',,
  '../src/avif/dav1d/src/mc_tmpl.c',,
  '../src/avif/dav1d/src/recon_tmpl.c',
)
foreach ($bitdepth in 8, 16) {
  foreach ($src in $dav1dTmplSources) {
    $obj = Join-Path $dav1dObjDir.FullName ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + (Split-Path $src -LeafBase) + '_' + $bitdepth + '.o')
    emcc -I../src/avif/dav1d -I../src/avif/dav1d/include -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -DBITDEPTH=$bitdepth -ffast-math -O3 -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "emcc failed for $src" }
    $dav1dObjs += $obj
  }
}

em++ `
  -I../include `
  -I../src `
  -I../src/webp `
  -I../src/avif/libavif/include `
  -I../src/avif/dav1d `
  -I../src/avif/dav1d/include `
  ../src/core/adaptive_chroma.cpp `
  ../src/core/avif_decode.cpp `
  ../src/core/cclm.cpp `
  ../src/core/coeff_presence_stream.cpp `
  ../src/core/coeff_sign_stream.cpp `
  ../src/core/coeff_span_stream.cpp `
  ../src/core/coeff_table_bank_stream.cpp `
  ../src/core/coeff_table_stream.cpp `
  ../src/core/colorspace.cpp `
  ../src/core/container.cpp `
  ../src/core/control_field.cpp `
  ../src/core/dct.cpp `
  ../src/core/decoder.cpp `
  ../src/core/encoder.cpp `
  ../src/core/exif_import.cpp `
  ../src/core/image_io.cpp `
  ../src/stb/stb_image.cpp `
  ../src/core/lossless.cpp `
  ../src/core/lossy_coeff_stream.cpp `
  ../src/core/lossy_tile_decode.cpp `
  ../src/core/lossy_tile_encode.cpp `
  ../src/core/metrics.cpp `
  ../src/core/mode_span_rans.cpp `
  ../src/core/mode_stream.cpp `
  ../src/core/predict.cpp `
  ../src/core/rans.cpp `
  ../src/core/superblock_quant.cpp `
  threading_stub.cpp `
  ../src/core/tile_util.cpp `
  ../src/core/webp_decode.cpp `
  ../src/core/wkmeta.cpp `
  $webpObjs `
  $avifObjs `
  $libyuvObjs `
  $dav1dObjs `
  wk_wasm.cpp `
  -o wk.js `
  -s WASM=1 `
  -s "EXPORTED_FUNCTIONS=[$exported_functions]" `
  -s "EXPORTED_RUNTIME_METHODS=[`"wasmMemory`"]" `
  -s ALLOW_MEMORY_GROWTH=1 `
  -O3 `
  -std=c++2b `
  -s MODULARIZE=1 `
  -s EXPORT_NAME="WkModule"

Remove-Item -Recurse -Force $webpObjDir
Remove-Item -Recurse -Force $avifObjDir
Remove-Item -Recurse -Force $libyuvObjDir
Remove-Item -Recurse -Force $dav1dObjDir
