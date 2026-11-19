#!/bin/bash
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

EXPORTED_FUNCTIONS="['_wk_wasm_alloc','_wk_wasm_free','_wk_wasm_decode','_wk_wasm_encode','_wk_wasm_version','_wk_wasm_get_info','_malloc','_free']"
EXPORTED_RUNTIME="['wasmMemory']"

# Source list: every src/core/*.cpp plus the stb implementation
# unit. src/arch/generic/threading.cpp is deliberately excluded:
# under __EMSCRIPTEN__ src/core/threading.h provides the inline
# single-threaded ThreadPool from src/arch/wasm, and the real pool in
# generic/threading.cpp would conflict with it. Keep this list in
# sync with src/ when modules are added.
# em++ (not emcc): the engine is C++ and the link needs the C++
# standard library (operator new/delete, std::string).
# Vendored libwebp 1.3.2 decode-only C sources (see src/webp/README.md):
# compiled with emcc (C), not em++ — em++ would compile them as C++ and
# libwebp is C. Listed explicitly, keep in sync with src/webp/.
# The objects are linked by em++ below, then removed.
WEBP_C_SOURCES="
../src/webp/src/dec/alpha_dec.c
../src/webp/src/dec/buffer_dec.c
../src/webp/src/dec/frame_dec.c
../src/webp/src/dec/io_dec.c
../src/webp/src/dec/quant_dec.c
../src/webp/src/dec/tree_dec.c
../src/webp/src/dec/vp8_dec.c
../src/webp/src/dec/vp8l_dec.c
../src/webp/src/dec/webp_dec.c
../src/webp/src/dsp/alpha_processing.c
../src/webp/src/dsp/alpha_processing_neon.c
../src/webp/src/dsp/alpha_processing_sse2.c
../src/webp/src/dsp/alpha_processing_sse41.c
../src/webp/src/dsp/cpu.c
../src/webp/src/dsp/dec.c
../src/webp/src/dsp/dec_clip_tables.c
../src/webp/src/dsp/dec_neon.c
../src/webp/src/dsp/dec_sse2.c
../src/webp/src/dsp/dec_sse41.c
../src/webp/src/dsp/filters.c
../src/webp/src/dsp/filters_neon.c
../src/webp/src/dsp/filters_sse2.c
../src/webp/src/dsp/lossless.c
../src/webp/src/dsp/lossless_neon.c
../src/webp/src/dsp/lossless_sse2.c
../src/webp/src/dsp/lossless_sse41.c
../src/webp/src/dsp/rescaler.c
../src/webp/src/dsp/rescaler_neon.c
../src/webp/src/dsp/rescaler_sse2.c
../src/webp/src/dsp/upsampling.c
../src/webp/src/dsp/upsampling_neon.c
../src/webp/src/dsp/upsampling_sse2.c
../src/webp/src/dsp/upsampling_sse41.c
../src/webp/src/dsp/yuv.c
../src/webp/src/dsp/yuv_neon.c
../src/webp/src/dsp/yuv_sse2.c
../src/webp/src/dsp/yuv_sse41.c
../src/webp/src/utils/bit_reader_utils.c
../src/webp/src/utils/color_cache_utils.c
../src/webp/src/utils/huffman_utils.c
../src/webp/src/utils/quant_levels_dec_utils.c
../src/webp/src/utils/random_utils.c
../src/webp/src/utils/rescaler_utils.c
../src/webp/src/utils/thread_utils.c
../src/webp/src/utils/utils.c
"
WEBP_OBJ_DIR="$(mktemp -d)"
trap 'rm -rf "$WEBP_OBJ_DIR"' EXIT
WEBP_OBJS=""
for webp_src in $WEBP_C_SOURCES; do
  webp_obj="$WEBP_OBJ_DIR/$(basename "$(dirname "$webp_src")")_$(basename "$webp_src" .c).o"
  emcc -I../src/webp -O3 -c "$webp_src" -o "$webp_obj" || exit 1
  WEBP_OBJS="$WEBP_OBJS $webp_obj"
done

em++ \
  -I../include \
  -I../src \
  -I../src/webp \
  ../src/core/adaptive_chroma.cpp \
  ../src/core/cclm.cpp \
  ../src/core/coeff_presence_stream.cpp \
  ../src/core/coeff_sign_stream.cpp \
  ../src/core/coeff_span_stream.cpp \
  ../src/core/coeff_table_bank_stream.cpp \
  ../src/core/coeff_table_stream.cpp \
  ../src/core/colorspace.cpp \
  ../src/core/container.cpp \
  ../src/core/control_field.cpp \
  ../src/core/dct.cpp \
  ../src/core/decoder.cpp \
  ../src/core/encoder.cpp \
  ../src/core/exif_import.cpp \
  ../src/core/image_io.cpp \
  ../src/stb/stb_image.cpp \
  ../src/core/lossless.cpp \
  ../src/core/lossy_coeff_stream.cpp \
  ../src/core/lossy_tile_decode.cpp \
  ../src/core/lossy_tile_encode.cpp \
  ../src/core/metrics.cpp \
  ../src/core/mode_span_rans.cpp \
  ../src/core/mode_stream.cpp \
  ../src/core/predict.cpp \
  ../src/core/rans.cpp \
  ../src/core/superblock_quant.cpp \
  threading_stub.cpp \
  ../src/core/tile_util.cpp \
  ../src/core/webp_decode.cpp \
  ../src/core/wkmeta.cpp \
  $WEBP_OBJS \
  wk_wasm.cpp \
  -o wk.js \
  -s WASM=1 \
  -s EXPORTED_FUNCTIONS="$EXPORTED_FUNCTIONS" \
  -s EXPORTED_RUNTIME_METHODS="$EXPORTED_RUNTIME" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -O3 \
  -std=c++2b \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="WkModule"
