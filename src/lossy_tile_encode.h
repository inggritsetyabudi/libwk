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

#include "common.h"
#pragma once

#include "common.h"
#include <wk/wk.hpp>
#include <cstdint>
#include <vector>

namespace wk {

struct TileEncodeResult {
    uint16_t tile_x;
    uint16_t tile_y;
    std::vector<uint8_t> compressed;
    float    quality_score;
    TileStats stats;  // filled only when the caller passes a stats sink
    ChromaSubsampling subsampling = ChromaSubsampling::YUV444;
    bool is_v11 = false;  // written with the V11 layout tag (see below)
};

Result<TileEncodeResult> encode_lossy_tile(
    const int16_t* y_plane, const int16_t* cb_plane, const int16_t* cr_plane,
    // Full-resolution chroma for the control-field classification.
    // cb_plane/cr_plane above are at the TILE's subsampling resolution
    // (half-res for a 420 tile); the classifier was measured on
    // full-res chroma, so the full planes are passed separately.
    const int16_t* cb_plane_full, const int16_t* cr_plane_full,
    const int16_t* alpha_plane,
    uint32_t image_width, uint32_t image_height,
    uint32_t tile_x, uint32_t tile_y, uint32_t tile_size,
    float quality, uint8_t bit_depth, bool has_alpha,
    ChromaSubsampling subsampling,
    bool use_control_field,
    bool use_cclm,
    bool adaptive_chroma_subsampling,
    TileStats* stats = nullptr);

}  // namespace wk
