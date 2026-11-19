// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <wk/wk.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern "C" {

EMSCRIPTEN_KEEPALIVE
void* wk_wasm_alloc(size_t size) {
    return malloc(size);
}

EMSCRIPTEN_KEEPALIVE
void wk_wasm_free(void* ptr) {
    free(ptr);
}

EMSCRIPTEN_KEEPALIVE
int wk_wasm_decode(
    const uint8_t* data, size_t size,
    uint32_t* out_width, uint32_t* out_height,
    uint32_t* out_bpp, uint32_t* out_pixels_ptr
) {
    uint8_t* out_pixels = nullptr;
    uint32_t w = 0, h = 0;
    uint8_t bpp = 0;

    wk_error_t err = wk_decode(data, size, &out_pixels, &w, &h, &bpp);

    *out_width = w;
    *out_height = h;
    *out_bpp = bpp;
    *out_pixels_ptr = (uint32_t)(uintptr_t)out_pixels;
    return (int)err;
}

// FHDR facts for the JS display path (web/app.js): bit depth + CICP
// + MaxCLL/MaxFALL, so JS can pick the right EOTF and label the
// panel with the real bit depth instead of guessing from bpp.
// Writes 15 uint32s into the caller-allocated buffer (60 bytes):
//   [0] width, [1] height, [2] bit_depth, [3] cicp_primaries,
//   [4] cicp_transfer, [5] cicp_matrix, [6] flags,
//   [7] tile_size_log2, [8] max_cll, [9] max_fall,
//   [10] has_alpha, [11] is_lossless, [12] is_animated,
//   [13] is_hdr, [14] frame_count.
// Fields are written one by one (not a struct copy) so no packing
// assumption crosses the wasm/JS boundary. Returns wk_error_t.
EMSCRIPTEN_KEEPALIVE
int wk_wasm_get_info(
    const uint8_t* data, size_t size,
    uint32_t* out_info
) {
    wk_image_info_t info;
    wk_error_t err = wk_get_info(data, size, &info);
    if (err != WK_OK) {
        return (int)err;
    }
    out_info[0] = info.width;
    out_info[1] = info.height;
    out_info[2] = info.bit_depth;
    out_info[3] = info.cicp_primaries;
    out_info[4] = info.cicp_transfer;
    out_info[5] = info.cicp_matrix;
    out_info[6] = info.flags;
    out_info[7] = info.tile_size_log2;
    out_info[8] = info.max_cll;
    out_info[9] = info.max_fall;
    out_info[10] = info.has_alpha;
    out_info[11] = info.is_lossless;
    out_info[12] = info.is_animated;
    out_info[13] = info.is_hdr;
    out_info[14] = info.frame_count;
    return (int)WK_OK;
}

EMSCRIPTEN_KEEPALIVE
int wk_wasm_encode(
    const uint8_t* pixels, uint32_t width, uint32_t height,
    uint8_t bpp, float quality, uint8_t lossless,
    uint32_t* out_size, uint32_t* out_data_ptr
) {
    wk_encoder_config_t config;
    wk_encoder_config_init(&config);
    config.quality = quality;
    config.lossless = lossless;
    config.threads = 1;

    uint8_t* out_data = nullptr;
    size_t sz = 0;
    wk_error_t err = wk_encode(
        pixels, width, height, width * bpp, bpp,
        &config, &out_data, &sz
    );

    *out_size = (uint32_t)sz;
    *out_data_ptr = (uint32_t)(uintptr_t)out_data;
    return (int)err;
}

EMSCRIPTEN_KEEPALIVE
const char* wk_wasm_version(void) {
    return wk_version();
}

}
