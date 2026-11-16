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
    '_malloc',
    '_free'
) -join ','

em++ `
  -I../include `
  -I../src `
  ../src/core/adaptive_chroma.cpp `
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
  ../src/core/wkmeta.cpp `
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
