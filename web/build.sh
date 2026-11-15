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

EXPORTED_FUNCTIONS="['_wk_wasm_alloc','_wk_wasm_free','_wk_wasm_decode','_wk_wasm_encode','_wk_wasm_version','_malloc','_free']"
EXPORTED_RUNTIME="['wasmMemory']"

# Source list: every src/*.cpp EXCEPT threading.cpp and the removed
# quantize.cpp. threading.cpp is deliberately excluded: under
# __EMSCRIPTEN__ threading.h provides an inline single-threaded
# ThreadPool, and the real pool in threading.cpp would conflict with
# it. Keep this list in sync with src/ when modules are added.
# em++ (not emcc): the engine is C++ and the link needs the C++
# standard library (operator new/delete, std::string).
em++ \
  -I../include \
  -I../src \
  ../src/adaptive_chroma.cpp \
  ../src/cclm.cpp \
  ../src/coeff_presence_stream.cpp \
  ../src/coeff_sign_stream.cpp \
  ../src/coeff_span_stream.cpp \
  ../src/coeff_table_bank_stream.cpp \
  ../src/coeff_table_stream.cpp \
  ../src/colorspace.cpp \
  ../src/container.cpp \
  ../src/control_field.cpp \
  ../src/dct.cpp \
  ../src/decoder.cpp \
  ../src/encoder.cpp \
  ../src/exif_import.cpp \
  ../src/image_io.cpp \
  ../src/lossless.cpp \
  ../src/lossy_coeff_stream.cpp \
  ../src/lossy_tile_decode.cpp \
  ../src/lossy_tile_encode.cpp \
  ../src/metrics.cpp \
  ../src/mode_span_rans.cpp \
  ../src/mode_stream.cpp \
  ../src/predict.cpp \
  ../src/rans.cpp \
  ../src/superblock_quant.cpp \
  threading_stub.cpp \
  ../src/tile_util.cpp \
  ../src/wkmeta.cpp \
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
