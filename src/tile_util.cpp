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
#include "tile_util.h"
#include <algorithm>

namespace wk {

Result<std::span<const uint8_t>> tile_payload_view(const Chunk& tile_chunk, TileHeader& header) {
    if (tile_chunk.payload.size() < 9) {
        return std::unexpected(Error{ErrorCode::InvalidChunkSize, "tile chunk shorter than header"});
    }

    auto parsed = parse_tile_header({tile_chunk.payload.data(), 9});
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    header = *parsed;

    const size_t payload_size = tile_chunk.payload.size() - 9;
    if (payload_size != header.compressed_size) {
        return std::unexpected(Error{ErrorCode::InvalidChunkSize,
            "tile payload size does not match compressed_size"});
    }

    return std::span<const uint8_t>(tile_chunk.payload.data() + 9, payload_size);
}

Result<TileGeometry> infer_tile_geometry(uint16_t tile_x, uint16_t tile_y,
                                          uint32_t tile_size,
                                          uint32_t image_width, uint32_t image_height,
                                          uint16_t blocks_x, uint16_t blocks_y,
                                          uint16_t chroma_blocks_x, uint16_t chroma_blocks_y,
                                          std::optional<ChromaSubsampling> preferred) {
    const uint32_t x0 = static_cast<uint32_t>(tile_x) * tile_size;
    const uint32_t y0 = static_cast<uint32_t>(tile_y) * tile_size;
    if (x0 >= image_width || y0 >= image_height) {
        return std::unexpected(Error{ErrorCode::InvalidTileCoord, "tile coordinates out of range"});
    }

    const uint32_t tw = std::min(tile_size, image_width - x0);
    const uint32_t th = std::min(tile_size, image_height - y0);
    const uint16_t expected_blocks_x = static_cast<uint16_t>((tw + 7) / 8);
    const uint16_t expected_blocks_y = static_cast<uint16_t>((th + 7) / 8);
    if (blocks_x != expected_blocks_x || blocks_y != expected_blocks_y) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "tile luma block dimensions are invalid"});
    }

    const uint32_t chroma_tw_444 = tw;
    const uint32_t chroma_th_444 = th;
    const uint16_t chroma_bx_444 = expected_blocks_x;
    const uint16_t chroma_by_444 = expected_blocks_y;

    const uint32_t chroma_tw_420 = (tw + 1) / 2;
    const uint32_t chroma_th_420 = (th + 1) / 2;
    const uint16_t chroma_bx_420 = static_cast<uint16_t>((chroma_tw_420 + 7) / 8);
    const uint16_t chroma_by_420 = static_cast<uint16_t>((chroma_th_420 + 7) / 8);

    const bool match_444 = chroma_blocks_x == chroma_bx_444 && chroma_blocks_y == chroma_by_444;
    const bool match_420 = chroma_blocks_x == chroma_bx_420 && chroma_blocks_y == chroma_by_420;
    if (!match_444 && !match_420) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "tile chroma block dimensions are invalid"});
    }

    TileGeometry geometry;
    geometry.x0 = x0;
    geometry.y0 = y0;
    geometry.tw = tw;
    geometry.th = th;

    if (preferred && ((match_444 && *preferred == ChromaSubsampling::YUV444) ||
                      (match_420 && *preferred == ChromaSubsampling::YUV420))) {
        geometry.subsampling = *preferred;
    } else if (match_444 && !match_420) {
        geometry.subsampling = ChromaSubsampling::YUV444;
    } else if (match_420 && !match_444) {
        geometry.subsampling = ChromaSubsampling::YUV420;
    } else {
        geometry.subsampling = preferred.value_or(ChromaSubsampling::YUV444);
    }

    if (geometry.subsampling == ChromaSubsampling::YUV420) {
        geometry.chroma_tw = chroma_tw_420;
        geometry.chroma_th = chroma_th_420;
    } else {
        geometry.chroma_tw = chroma_tw_444;
        geometry.chroma_th = chroma_th_444;
    }

    return geometry;
}

Result<ChromaSubsampling> infer_subsampling_from_tile(std::span<const uint8_t> tile_data,
                                                       uint16_t tile_x, uint16_t tile_y,
                                                       uint32_t tile_size,
                                                       uint32_t image_width, uint32_t image_height) {
    ByteReader reader(tile_data);
    auto quant_data = reader.read_bytes(64 * sizeof(uint16_t) * 2);
    if (!quant_data) {
        return std::unexpected(quant_data.error());
    }

    auto blocks_x = reader.read_u16();
    auto blocks_y = reader.read_u16();
    auto chroma_blocks_x = reader.read_u16();
    auto chroma_blocks_y = reader.read_u16();
    if (!blocks_x || !blocks_y || !chroma_blocks_x || !chroma_blocks_y) {
        return std::unexpected(Error{ErrorCode::TruncatedInput, "missing tile block dimensions"});
    }

    auto geometry = infer_tile_geometry(tile_x, tile_y, tile_size, image_width, image_height,
                                        *blocks_x, *blocks_y, *chroma_blocks_x, *chroma_blocks_y);
    if (!geometry) {
        return std::unexpected(geometry.error());
    }
    return geometry->subsampling;
}

}  // namespace wk
