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


#ifndef WK_H
#define WK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        WK_OK = 0,
        WK_ERROR_INVALID_MAGIC = 1,
        WK_ERROR_INVALID_DATA = 2,
        WK_ERROR_TRUNCATED = 3,
        WK_ERROR_UNSUPPORTED = 4,
        WK_ERROR_ENCODE_FAIL = 5,
        WK_ERROR_DECODE_FAIL = 6,
        WK_ERROR_INVALID_PARAM = 7,
        WK_ERROR_IO = 8,
        WK_ERROR_OOM = 9,
        WK_ERROR_META_PARSE = 10,
        WK_ERROR_META_NOT_FOUND = 11,
    } wk_error_t;

    typedef struct
    {
        uint32_t width;
        uint32_t height;
        uint8_t bit_depth;
        uint8_t cicp_primaries;
        uint8_t cicp_transfer;
        uint8_t cicp_matrix;
        uint16_t flags;
        uint8_t tile_size_log2;
        uint16_t max_cll;
        uint16_t max_fall;
        uint8_t has_alpha;
        uint8_t is_lossless;
        uint8_t is_animated;
        uint8_t is_hdr;
        uint32_t frame_count;
    } wk_image_info_t;

    typedef struct
    {
        float quality;
        uint8_t lossless;
        uint8_t bit_depth;
        uint8_t cicp_primaries;
        uint8_t cicp_transfer;
        uint8_t cicp_matrix;
        uint8_t tile_size_log2;
        uint8_t threads;
        float target_ssimulacra2;  // Reserved: accepted but currently ignored.
        uint8_t chroma_subsampling;
        uint8_t full_range;
    } wk_encoder_config_t;

    void wk_encoder_config_init(wk_encoder_config_t *config);

    wk_error_t wk_encode(
        const uint8_t *pixels,
        uint32_t width,
        uint32_t height,
        uint32_t pixel_stride,
        uint8_t bpp,
        const wk_encoder_config_t *config,
        uint8_t **out_data,
        size_t *out_size);

    wk_error_t wk_get_info(
        const uint8_t *data,
        size_t size,
        wk_image_info_t *info);

    wk_error_t wk_decode(
        const uint8_t *data,
        size_t size,
        uint8_t **out_pixels,
        uint32_t *out_width,
        uint32_t *out_height,
        uint8_t *out_bpp);

    void wk_free(void *ptr);

    const char *wk_version(void);

#ifdef __cplusplus
}
#endif

#endif
