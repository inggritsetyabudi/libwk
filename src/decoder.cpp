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
#include "common.h"
#include "coeff_span_stream.h"
#include "container.h"
#include "dct.h"
#include "lossy_coeff_stream.h"
#include "mode_span_rans.h"
#include "mode_stream.h"
#include "predict.h"
#include "quantize.h"
#include "superblock_quant.h"
#include "colorspace.h"
#include "lossless.h"
#include "tile_util.h"
#include "lossy_tile_decode.h"
#include <wk/wk.hpp>
#include <wk/wk.h>
#include <cmath>
#include <cstring>
#include <exception>
#include <new>
#include <optional>

namespace wk {


Result<ImageInfo> get_info_impl(std::span<const uint8_t> data) {
    auto file = parse_container(data);
    if (!file) return std::unexpected(file.error());

    ImageInfo info;
    info.width = file->header.width;
    info.height = file->header.height;
    info.bit_depth = static_cast<BitDepth>(file->header.bit_depth);
    info.cicp = {file->header.cicp_primaries, file->header.cicp_transfer,
                 file->header.cicp_matrix, file->header.full_range()};
    info.has_alpha = file->header.has_alpha();
    info.is_lossless = file->header.is_lossless();
    info.is_animated = file->header.is_animated();
    info.is_hdr = file->header.is_hdr();
    info.has_wkmeta = file->header.has_wkmeta();
    info.tile_size = file->header.tile_size();
    info.max_cll = file->header.max_cll;
    info.max_fall = file->header.max_fall;
    info.frame_count = file->animation ? file->animation->frame_count : 1;
    return info;
}

// Peeks the lossy layout tag of a tile payload (the quant tables plus
// the block dimensions precede it). Returns nullopt when the payload
// is too short to hold a tag; the tile decode reports that truncation.
[[nodiscard]] std::optional<uint32_t> peek_lossy_layout_tag(std::span<const uint8_t> tile_data) {
    constexpr size_t kLayoutTagOffset = 64 * sizeof(uint16_t) * 2 + 4 * sizeof(uint16_t);
    if (tile_data.size() < kLayoutTagOffset + sizeof(uint32_t)) {
        return std::nullopt;
    }
    uint32_t tag = 0;
    std::memcpy(&tag, tile_data.data() + kLayoutTagOffset, sizeof(tag));
    return tag;
}

Result<Image> decode_impl(std::span<const uint8_t> data, const DecoderConfig& config) {
    auto file = parse_container(data);
    if (!file) return std::unexpected(file.error());

    const auto& hdr = file->header;
    const uint32_t width = hdr.width;
    const uint32_t height = hdr.height;

    if (config.info_only) {
        return Image(width, height, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    }

    Image output(width, height, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    if (hdr.is_lossless()) {
        // The lossless path decodes a single tile holding the whole
        // frame: extra tiles would be silently ignored if accepted.
        if (file->tile_chunks.size() != 1) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "lossless image must have exactly one tile"});
        }

        TileHeader header;
        auto tile_data = tile_payload_view(file->tile_chunks.front(), header);
        if (!tile_data) return std::unexpected(tile_data.error());

        auto decoded = lossless_decode(*tile_data, width, height, hdr.bit_depth);
        if (!decoded) return std::unexpected(decoded.error());

        if (hdr.has_alpha()) {
            std::memcpy(output.pixels().data(), decoded->data(),
                        std::min(decoded->size(), output.pixels().size()));
        } else {
            const size_t pixel_count = static_cast<size_t>(width) * height;
            for (size_t i = 0; i < pixel_count; ++i) {
                output.pixels()[i * 3 + 0] = (*decoded)[i * 4 + 0];
                output.pixels()[i * 3 + 1] = (*decoded)[i * 4 + 1];
                output.pixels()[i * 3 + 2] = (*decoded)[i * 4 + 2];
            }
        }
        return output;
    }

    if (file->tile_chunks.empty()) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "no tile chunks"});
    }

    // A lossy frame must be covered by its TILE chunks exactly once:
    // a missing tile used to decode as a blank quadrant and a duplicated
    // tile silently overwrote the first copy. Check before decoding
    // anything, so a malformed file fails fast instead of after work.
    // Animated files are excluded: their frames reuse tile coordinates
    // across frames, so grid coverage does not apply to them (animation
    // is unfinished -- the encoder never produces it and decode ignores
    // it -- so this stays out of scope rather than guessing its layout).
    if (!hdr.is_animated()) {
        const uint32_t tiles_x = hdr.tiles_x();
        const uint32_t tiles_y = hdr.tiles_y();
        if (file->tile_chunks.size() != hdr.tile_count()) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "tile chunk count does not match frame grid"});
        }
        std::vector<char> seen(static_cast<size_t>(tiles_x) * tiles_y, 0);
        for (const auto& tile_chunk : file->tile_chunks) {
            TileHeader tile_header;
            auto tile_data = tile_payload_view(tile_chunk, tile_header);
            if (!tile_data) return std::unexpected(tile_data.error());
            if (tile_header.tile_x >= tiles_x || tile_header.tile_y >= tiles_y) {
                return std::unexpected(Error{ErrorCode::InvalidTileCoord,
                                             "tile coordinates out of range"});
            }
            const size_t index = static_cast<size_t>(tile_header.tile_y) * tiles_x +
                                 tile_header.tile_x;
            if (seen[index]) {
                return std::unexpected(Error{ErrorCode::InvalidTileCoord,
                                             "duplicate tile chunk"});
            }
            seen[index] = 1;
        }
        // Chunk count equals the grid size and every coordinate is unique,
        // so the grid is fully covered by the pigeonhole principle.
    }

    const uint32_t tile_size = hdr.tile_size();
    TileHeader first_tile_header;
    auto first_tile_payload = tile_payload_view(file->tile_chunks.front(), first_tile_header);
    if (!first_tile_payload) return std::unexpected(first_tile_payload.error());

    // Image-wide fallback subsampling, inferred from the first tile as
    // before. It is only the preferred value for pre-V11 tiles (whose
    // block counts are the sole signal); V11 tiles take their
    // subsampling from the tile header flag instead, so a mixed file
    // needs no single image-wide subsampling.
    auto subsampling = infer_subsampling_from_tile(*first_tile_payload, first_tile_header.tile_x,
                                                   first_tile_header.tile_y, tile_size,
                                                   width, height);
    if (!subsampling) return std::unexpected(subsampling.error());

    // A file is mixed (per-tile subsampling) when any tile uses the
    // V11 layout. Legacy files take the old path below, so they decode
    // bit-identically (one global upsample); mixed files composite
    // chroma at full resolution, upsampling each 420 tile to its own
    // region (SPEC v0.2.6).
    bool has_v11_tile = false;
    for (const auto& tile_chunk : file->tile_chunks) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(tile_chunk, tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());
        if (peek_lossy_layout_tag(*tile_data) == kLossyTileLayoutTagV11) {
            has_v11_tile = true;
            break;
        }
    }

    const int16_t max_val = static_cast<int16_t>((1 << hdr.bit_depth) - 1);
    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<int16_t> full_y(pixel_count, 0);

    const uint32_t chroma_w = has_v11_tile ? width
        : (*subsampling == ChromaSubsampling::YUV420 ? (width + 1) / 2 : width);
    const uint32_t chroma_h = has_v11_tile ? height
        : (*subsampling == ChromaSubsampling::YUV420 ? (height + 1) / 2 : height);
    std::vector<int16_t> full_cb(static_cast<size_t>(chroma_w) * chroma_h, max_val / 2);
    std::vector<int16_t> full_cr(static_cast<size_t>(chroma_w) * chroma_h, max_val / 2);
    std::vector<int16_t> full_a;
    if (hdr.has_alpha()) {
        full_a.assign(pixel_count, max_val);
    }

    for (const auto& tile_chunk : file->tile_chunks) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(tile_chunk, tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());

        auto tile = decode_lossy_tile(*tile_data, tile_header.tile_x, tile_header.tile_y,
                                      tile_size, width, height, hdr.bit_depth, *subsampling,
                                      tile_header.layer_flags,
                                      hdr.has_alpha(), (tile_header.layer_flags & TILE_HAS_ALPHA) != 0);
        if (!tile) return std::unexpected(tile.error());

        for (uint32_t row = 0; row < tile->geometry.th; ++row) {
            for (uint32_t col = 0; col < tile->geometry.tw; ++col) {
                full_y[(tile->geometry.y0 + row) * width + (tile->geometry.x0 + col)] =
                    tile->y_plane[row * tile->geometry.tw + col];
            }
        }

        if (hdr.has_alpha()) {
            for (uint32_t row = 0; row < tile->geometry.th; ++row) {
                for (uint32_t col = 0; col < tile->geometry.tw; ++col) {
                    full_a[(tile->geometry.y0 + row) * width + (tile->geometry.x0 + col)] =
                        tile->alpha_plane[row * tile->geometry.tw + col];
                }
            }
        }

        if (has_v11_tile) {
            if (tile->geometry.subsampling == ChromaSubsampling::YUV420) {
                // Upsample this tile's chroma to its full-resolution
                // region (same bilinear filter as the image-wide
                // upsample, clamped to the tile's own chroma plane),
                // then copy it in.
                std::vector<int16_t> cb_up(static_cast<size_t>(tile->geometry.tw) * tile->geometry.th);
                std::vector<int16_t> cr_up(static_cast<size_t>(tile->geometry.tw) * tile->geometry.th);
                upsample_420(tile->cb_plane.data(), cb_up.data(), tile->geometry.tw, tile->geometry.th);
                upsample_420(tile->cr_plane.data(), cr_up.data(), tile->geometry.tw, tile->geometry.th);
                for (uint32_t row = 0; row < tile->geometry.th; ++row) {
                    for (uint32_t col = 0; col < tile->geometry.tw; ++col) {
                        full_cb[(tile->geometry.y0 + row) * width + (tile->geometry.x0 + col)] =
                            cb_up[row * tile->geometry.tw + col];
                        full_cr[(tile->geometry.y0 + row) * width + (tile->geometry.x0 + col)] =
                            cr_up[row * tile->geometry.tw + col];
                    }
                }
            } else {
                for (uint32_t row = 0; row < tile->geometry.chroma_th; ++row) {
                    for (uint32_t col = 0; col < tile->geometry.chroma_tw; ++col) {
                        full_cb[(tile->geometry.y0 + row) * width + (tile->geometry.x0 + col)] =
                            tile->cb_plane[row * tile->geometry.chroma_tw + col];
                        full_cr[(tile->geometry.y0 + row) * width + (tile->geometry.x0 + col)] =
                            tile->cr_plane[row * tile->geometry.chroma_tw + col];
                    }
                }
            }
        } else {
            const uint32_t chroma_x0 = *subsampling == ChromaSubsampling::YUV420 ? tile->geometry.x0 / 2 : tile->geometry.x0;
            const uint32_t chroma_y0 = *subsampling == ChromaSubsampling::YUV420 ? tile->geometry.y0 / 2 : tile->geometry.y0;
            for (uint32_t row = 0; row < tile->geometry.chroma_th; ++row) {
                for (uint32_t col = 0; col < tile->geometry.chroma_tw; ++col) {
                    full_cb[(chroma_y0 + row) * chroma_w + (chroma_x0 + col)] =
                        tile->cb_plane[row * tile->geometry.chroma_tw + col];
                    full_cr[(chroma_y0 + row) * chroma_w + (chroma_x0 + col)] =
                        tile->cr_plane[row * tile->geometry.chroma_tw + col];
                }
            }
        }
    }

    std::vector<int16_t> cb_full(pixel_count);
    std::vector<int16_t> cr_full(pixel_count);
    if (!has_v11_tile && *subsampling == ChromaSubsampling::YUV420) {
        upsample_420(full_cb.data(), cb_full.data(), width, height);
        upsample_420(full_cr.data(), cr_full.data(), width, height);
    } else {
        cb_full = std::move(full_cb);
        cr_full = std::move(full_cr);
    }

    ycbcr_to_rgb(output.pixels().data(), full_y.data(), cb_full.data(), cr_full.data(),
                 width, height, hdr.cicp_matrix, hdr.bit_depth, hdr.full_range(),
                 hdr.has_alpha(), hdr.has_alpha() ? full_a.data() : nullptr);
    return output;
}

// Public API boundary: decode works on untrusted files, and allocation
// failure (std::bad_alloc / std::length_error) must surface as a Result
// error, never escape as an exception -- callers in C (wk_decode) and
// tools have no handler, so an escaping exception would terminate the
// process (SPEC section 10: the decoder must never crash on malformed
// input). The checks above already bound the known huge allocations;
// this is the last line of defense for any allocation left.
Result<ImageInfo> get_info(std::span<const uint8_t> data) {
    try {
        return get_info_impl(data);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while reading info"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, std::string("exception while reading info: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unknown exception while reading info"});
    }
}

Result<Image> decode(std::span<const uint8_t> data, const DecoderConfig& config) {
    try {
        return decode_impl(data, config);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while decoding"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, std::string("exception while decoding: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unknown exception while decoding"});
    }
}

}

extern "C" {

wk_error_t wk_decode(
    const uint8_t* data, size_t size,
    uint8_t** out_pixels, uint32_t* out_width, uint32_t* out_height,
    uint8_t* out_bpp) {

    if (!data || !out_pixels || !out_width || !out_height || !out_bpp) {
        return WK_ERROR_INVALID_PARAM;
    }

    auto result = wk::decode({data, size});
    if (!result) return WK_ERROR_DECODE_FAIL;

    *out_width = result->width();
    *out_height = result->height();
    *out_bpp = static_cast<uint8_t>(result->bytes_per_pixel());

    size_t total = result->pixels().size();
    *out_pixels = static_cast<uint8_t*>(std::malloc(total));
    if (!*out_pixels) return WK_ERROR_OOM;
    std::memcpy(*out_pixels, result->pixels().data(), total);

    return WK_OK;
}

}

