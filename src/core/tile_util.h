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

#include "cclm.h"
#pragma once

#include "common.h"
#include "container.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wk {

struct TileGeometry {
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t tw = 0;
    uint32_t th = 0;
    uint32_t chroma_tw = 0;
    uint32_t chroma_th = 0;
    ChromaSubsampling subsampling = ChromaSubsampling::YUV444;
};

struct TileDecodeResult {
    uint16_t tile_x = 0;
    uint16_t tile_y = 0;
    std::vector<int16_t> y_plane;
    std::vector<int16_t> cb_plane;
    std::vector<int16_t> cr_plane;
    std::vector<int16_t> alpha_plane;
    TileGeometry geometry;
};

Result<std::span<const uint8_t>> tile_payload_view(const Chunk& tile_chunk, TileHeader& header);

Result<TileGeometry> infer_tile_geometry(uint16_t tile_x, uint16_t tile_y,
                                          uint32_t tile_size,
                                          uint32_t image_width, uint32_t image_height,
                                          uint16_t blocks_x, uint16_t blocks_y,
                                          uint16_t chroma_blocks_x, uint16_t chroma_blocks_y,
                                          std::optional<ChromaSubsampling> preferred = std::nullopt);

Result<ChromaSubsampling> infer_subsampling_from_tile(std::span<const uint8_t> tile_data,
                                                       uint16_t tile_x, uint16_t tile_y,
                                                       uint32_t tile_size,
                                                       uint32_t image_width, uint32_t image_height);

}  // namespace wk
