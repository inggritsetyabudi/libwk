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
#include <wk/wk.hpp>
#include <wk/wk.h>
#include <cmath>
#include <cstring>
#include <exception>
#include <new>
#include <optional>

namespace wk {

namespace {

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
                                          std::optional<ChromaSubsampling> preferred = std::nullopt) {
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

Result<TileDecodeResult> decode_lossy_tile(std::span<const uint8_t> tile_data,
                                            uint16_t tile_x, uint16_t tile_y,
                                            uint32_t tile_size,
                                            uint32_t image_width, uint32_t image_height,
                                            uint8_t bit_depth,
                                            ChromaSubsampling subsampling,
                                            bool has_alpha,
                                            bool tile_has_alpha) {
    ByteReader reader(tile_data);
    const int16_t max_val = static_cast<int16_t>((1 << bit_depth) - 1);

    std::array<uint16_t, 64> y_steps{};
    std::array<uint16_t, 64> c_steps{};
    for (int i = 0; i < 64; ++i) {
        auto value = reader.read_u16();
        if (!value) return std::unexpected(value.error());
        y_steps[i] = *value;
    }
    for (int i = 0; i < 64; ++i) {
        auto value = reader.read_u16();
        if (!value) return std::unexpected(value.error());
        c_steps[i] = *value;
    }

    auto blocks_x_read = reader.read_u16();
    auto blocks_y_read = reader.read_u16();
    auto chroma_blocks_x_read = reader.read_u16();
    auto chroma_blocks_y_read = reader.read_u16();
    if (!blocks_x_read || !blocks_y_read || !chroma_blocks_x_read || !chroma_blocks_y_read) {
        return std::unexpected(Error{ErrorCode::TruncatedInput, "missing tile block dimensions"});
    }

    const uint16_t blocks_x = *blocks_x_read;
    const uint16_t blocks_y = *blocks_y_read;
    const uint16_t chroma_blocks_x = *chroma_blocks_x_read;
    const uint16_t chroma_blocks_y = *chroma_blocks_y_read;

    auto layout_tag = reader.read_u32();
    if (!layout_tag) {
        return std::unexpected(Error{ErrorCode::TruncatedInput, "missing lossy tile layout tag"});
    }
    // V7 behaves exactly like V6 for every syntax feature; it only adds
    // the per-block quant-scale streams after the syntax-flags byte.
    // V8 is V7 plus rANS-coded modes and spans (SPEC 3.2/6).
    // V9 is V8 plus the superblock quant map after the scale streams.
    const bool is_v7 = *layout_tag == kLossyTileLayoutTagV7;
    const bool is_v8 = *layout_tag == kLossyTileLayoutTagV8;
    const bool is_v9 = *layout_tag == kLossyTileLayoutTagV9;
    const bool is_v10 = *layout_tag == kLossyTileLayoutTagV10;
    const bool is_v6_family = *layout_tag == kLossyTileLayoutTagV6 || is_v7 || is_v8 || is_v9 || is_v10;
    const bool has_scale_streams = is_v7 || is_v8 || is_v9 || is_v10;
    const bool packed_mode_streams = *layout_tag == kLossyTileLayoutTagV2 ||
                                     *layout_tag == kLossyTileLayoutTagV3 ||
                                     *layout_tag == kLossyTileLayoutTagV4 ||
                                     *layout_tag == kLossyTileLayoutTagV5 ||
                                     *layout_tag == kLossyTileLayoutTagV6 || is_v7;
    const bool raw_mode_streams = *layout_tag == kLossyTileLayoutTagV1;
    const bool packed_coefficient_spans = *layout_tag == kLossyTileLayoutTagV3 ||
                                          *layout_tag == kLossyTileLayoutTagV4 ||
                                          *layout_tag == kLossyTileLayoutTagV5 ||
                                          *layout_tag == kLossyTileLayoutTagV6 || is_v7;
    const bool adaptive_coefficient_tables = *layout_tag == kLossyTileLayoutTagV4 ||
                                             *layout_tag == kLossyTileLayoutTagV5 ||
                                             is_v6_family;
    if (!packed_mode_streams && !raw_mode_streams && !is_v8 && !is_v9 && !is_v10) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unsupported lossy tile layout"});
    }

    uint8_t syntax_flags = 0;
    if (is_v6_family) {
        auto flags = reader.read_u8();
        if (!flags) {
            return std::unexpected(Error{ErrorCode::TruncatedInput, "missing lossy tile syntax flags"});
        }
        syntax_flags = *flags;
        if ((syntax_flags & ~(kLossyTileSyntaxFlagAdaptiveSpanStreams |
                              kLossyTileSyntaxFlagPlaneCoeffExtents |
                              kLossyTileSyntaxFlagSplitMagnitudeSigns |
                              kLossyTileSyntaxFlagSharedChromaCoeffTables |
                              kLossyTileSyntaxFlagCoefficientTableBank |
                              kLossyTileSyntaxFlagElideSingleSymbolStreams |
                              kLossyTileSyntaxFlagCoefficientSignificanceMaps |
                              kLossyTileSyntaxFlagAdaptiveCoefficientSigns)) != 0) {
            return std::unexpected(Error{ErrorCode::DecodeFailed, "lossy tile syntax flags are invalid"});
        }
    }

    // V7 per-block quant scales (SPEC 3.2): sparse lists, one per plane,
    // read immediately after the syntax-flags byte. Strictly validated:
    // count within the plane's block count, indices in range and
    // strictly increasing, scale index within the factor table.
    auto read_quant_scale_stream = [&](size_t block_count, std::string_view label)
        -> Result<std::vector<float>> {
        std::vector<float> scales(block_count, 1.0f);
        auto count = reader.read_u16();
        if (!count) {
            return std::unexpected(count.error());
        }
        if (*count > block_count) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                std::string("quant scale count exceeds ") + std::string(label) + " block count"});
        }
        size_t prev_index = 0;
        bool first = true;
        for (uint16_t i = 0; i < *count; ++i) {
            auto index = reader.read_u16();
            if (!index) {
                return std::unexpected(index.error());
            }
            auto scale = reader.read_u8();
            if (!scale) {
                return std::unexpected(scale.error());
            }
            if (*index >= block_count) {
                return std::unexpected(Error{ErrorCode::DecodeFailed,
                    std::string("quant scale block index out of range in ") + std::string(label)});
            }
            if (!first && *index <= prev_index) {
                return std::unexpected(Error{ErrorCode::DecodeFailed,
                    std::string("quant scale block indices not strictly increasing in ") + std::string(label)});
            }
            if (*scale >= QuantTable::kQuantScaleFactors.size()) {
                return std::unexpected(Error{ErrorCode::DecodeFailed,
                    std::string("quant scale index out of range in ") + std::string(label)});
            }
            scales[*index] = QuantTable::kQuantScaleFactors[*scale];
            prev_index = *index;
            first = false;
        }
        return scales;
    };

    std::vector<float> y_scales;
    std::vector<float> c_scales;
    if (has_scale_streams) {
        auto y = read_quant_scale_stream(static_cast<size_t>(blocks_x) * blocks_y, "luma");
        if (!y) {
            return std::unexpected(y.error());
        }
        y_scales = std::move(*y);
        auto c = read_quant_scale_stream(static_cast<size_t>(chroma_blocks_x) * chroma_blocks_y, "chroma");
        if (!c) {
            return std::unexpected(c.error());
        }
        c_scales = std::move(*c);
    }

    // V9/V10 superblock quant map (SPEC 3.2): one sparse list, shared
    // by luma and chroma, read immediately after the per-block scale
    // streams. Effective scale for a block: its per-block scale when
    // non-identity, else its superblock's scale, else 1.0. V10 always
    // carries the map section (it may be empty, count 0); V9 carries
    // it because the map is nontrivial there by construction.
    std::vector<uint8_t> superblock_scales;
    if (is_v9 || is_v10) {
        const size_t superblock_count =
            static_cast<size_t>(superblocks_x(blocks_x)) * superblocks_y(blocks_y);
        auto map = read_superblock_quant_map(reader, superblock_count);
        if (!map) {
            return std::unexpected(map.error());
        }
        superblock_scales = std::move(*map);
    }
    const bool adaptive_span_streams = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagAdaptiveSpanStreams) != 0;
    const bool split_magnitude_signs = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagSplitMagnitudeSigns) != 0;
    const bool shared_chroma_coeff_tables = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagSharedChromaCoeffTables) != 0;
    const bool coefficient_table_bank = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagCoefficientTableBank) != 0;
    const bool elide_single_symbol_streams = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagElideSingleSymbolStreams) != 0;
    const bool coefficient_significance_maps = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagCoefficientSignificanceMaps) != 0;
    const bool adaptive_coefficient_signs = is_v6_family &&
        (syntax_flags & kLossyTileSyntaxFlagAdaptiveCoefficientSigns) != 0;
    const bool explicit_plane_max_coeff_spans = *layout_tag == kLossyTileLayoutTagV5 ||
        (is_v6_family &&
         (syntax_flags & kLossyTileSyntaxFlagPlaneCoeffExtents) != 0);

    auto geometry = infer_tile_geometry(tile_x, tile_y, tile_size, image_width, image_height,
                                        blocks_x, blocks_y, chroma_blocks_x, chroma_blocks_y,
                                        subsampling);
    if (!geometry) {
        return std::unexpected(geometry.error());
    }

    const size_t num_y_blocks = static_cast<size_t>(blocks_x) * blocks_y;
    const size_t num_c_blocks = static_cast<size_t>(chroma_blocks_x) * chroma_blocks_y;
    auto read_raw_modes = [&](size_t mode_count, std::string_view label) -> Result<std::vector<PredMode>> {
        std::vector<PredMode> modes(mode_count);
        for (size_t i = 0; i < mode_count; ++i) {
            auto mode = reader.read_u8();
            if (!mode) {
                return std::unexpected(mode.error());
            }
            if (*mode >= static_cast<uint8_t>(PredMode::NUM_MODES)) {
                return std::unexpected(
                    Error{ErrorCode::PredictionError,
                          std::string("invalid ") + std::string(label) + " prediction mode"});
            }
            modes[i] = static_cast<PredMode>(*mode);
        }
        return modes;
    };

    auto y_modes = (is_v8 || is_v9 || is_v10) ? read_rans_prediction_modes(reader, num_y_blocks, "luma")
                         : packed_mode_streams ? read_packed_prediction_modes(reader, num_y_blocks, "luma")
                                               : read_raw_modes(num_y_blocks, "luma");
    if (!y_modes) {
        return std::unexpected(y_modes.error());
    }

    auto chroma_modes = (is_v8 || is_v9 || is_v10) ? read_rans_prediction_modes(reader, num_c_blocks, "chroma")
                              : packed_mode_streams ? read_packed_prediction_modes(reader, num_c_blocks, "chroma")
                                                    : read_raw_modes(num_c_blocks, "chroma");
    if (!chroma_modes) {
        return std::unexpected(chroma_modes.error());
    }

    // CCLM (mode 14) is chroma-only and only valid in layout V10+
    // (SPEC 6). Reject it in the luma stream, and in any layout below
    // V10 (an old writer never emits it, so its presence there means
    // a corrupt or hostile stream).
    {
        auto uses_cclm = [](const std::vector<PredMode>& modes) {
            for (PredMode mode : modes) {
                if (mode == PredMode::CCLM) return true;
            }
            return false;
        };
        if (uses_cclm(*y_modes)) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "CCLM prediction mode is chroma-only"});
        }
        if (!is_v10 && uses_cclm(*chroma_modes)) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "CCLM prediction mode requires layout V10"});
        }
    }

    // PLANAR (mode 13) is only valid in layout V7+ (SPEC 6).
    if (!has_scale_streams) {
        auto uses_planar = [](const std::vector<PredMode>& modes) {
            for (PredMode mode : modes) {
                if (mode == PredMode::PLANAR) {
                    return true;
                }
            }
            return false;
        };
        if (uses_planar(*y_modes) || uses_planar(*chroma_modes)) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "planar prediction mode requires layout V7"});
        }
    }

    std::vector<uint8_t> y_spans(num_y_blocks, 64);
    if (is_v8 || is_v9 || is_v10) {
        auto spans = read_rans_coefficient_spans(reader, num_y_blocks, "luma");
        if (!spans) {
            return std::unexpected(spans.error());
        }
        y_spans = std::move(*spans);
    } else if (packed_coefficient_spans) {
        auto spans = adaptive_span_streams
            ? read_adaptive_coefficient_spans(reader, num_y_blocks, "luma")
            : read_packed_coefficient_spans(reader, num_y_blocks, "luma");
        if (!spans) {
            return std::unexpected(spans.error());
        }
        y_spans = std::move(*spans);
    }

    std::vector<uint8_t> chroma_spans(num_c_blocks, 64);
    if (is_v8 || is_v9 || is_v10) {
        auto spans = read_rans_coefficient_spans(reader, num_c_blocks, "chroma");
        if (!spans) {
            return std::unexpected(spans.error());
        }
        chroma_spans = std::move(*spans);
    } else if (packed_coefficient_spans) {
        auto spans = adaptive_span_streams
            ? read_adaptive_coefficient_spans(reader, num_c_blocks, "chroma")
            : read_packed_coefficient_spans(reader, num_c_blocks, "chroma");
        if (!spans) {
            return std::unexpected(spans.error());
        }
        chroma_spans = std::move(*spans);
    }

    uint8_t y_max_coeff_span = 64;
    uint8_t chroma_max_coeff_span = 64;
    if (explicit_plane_max_coeff_spans) {
        auto y_max = reader.read_u8();
        auto chroma_max = reader.read_u8();
        if (!y_max || !chroma_max) {
            return std::unexpected(Error{ErrorCode::TruncatedInput,
                                         "missing plane coefficient extents"});
        }
        y_max_coeff_span = *y_max;
        chroma_max_coeff_span = *chroma_max;
    }

    if (y_max_coeff_span > 64 || chroma_max_coeff_span > 64) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
                                     "plane coefficient extent is out of range"});
    }
    for (uint8_t span_value : y_spans) {
        if (span_value > y_max_coeff_span) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "luma coefficient span exceeds plane extent"});
        }
    }
    for (uint8_t span_value : chroma_spans) {
        if (span_value > chroma_max_coeff_span) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "chroma coefficient span exceeds plane extent"});
        }
    }

    const LossyCoeffStreamConfig coeff_config{
        .use_plane_max_coeff_span = explicit_plane_max_coeff_spans,
        .adaptive_coefficient_tables = adaptive_coefficient_tables,
        .split_magnitude_signs = split_magnitude_signs,
        .use_table_bank = coefficient_table_bank,
        .elide_single_symbol_streams = elide_single_symbol_streams,
        .use_significance_maps = coefficient_significance_maps,
        .use_adaptive_sign_streams = adaptive_coefficient_signs,
        .bit_depth = bit_depth,
    };

    auto y_coeffs = decode_lossy_plane_payload(reader, y_spans, y_max_coeff_span, coeff_config);
    if (!y_coeffs) return std::unexpected(y_coeffs.error());

    std::vector<DctBlockI16> cb_coeffs;
    std::vector<DctBlockI16> cr_coeffs;
    if (shared_chroma_coeff_tables) {
        auto chroma_coeffs = decode_lossy_chroma_payload(reader, chroma_spans, chroma_max_coeff_span, coeff_config);
        if (!chroma_coeffs) {
            return std::unexpected(chroma_coeffs.error());
        }
        cb_coeffs = std::move(chroma_coeffs->cb_blocks);
        cr_coeffs = std::move(chroma_coeffs->cr_blocks);
    } else {
        auto cb_plane = decode_lossy_plane_payload(reader, chroma_spans, chroma_max_coeff_span, coeff_config);
        if (!cb_plane) return std::unexpected(cb_plane.error());
        auto cr_plane = decode_lossy_plane_payload(reader, chroma_spans, chroma_max_coeff_span, coeff_config);
        if (!cr_plane) return std::unexpected(cr_plane.error());
        cb_coeffs = std::move(*cb_plane);
        cr_coeffs = std::move(*cr_plane);
    }

    TileDecodeResult result;
    result.tile_x = tile_x;
    result.tile_y = tile_y;
    result.geometry = *geometry;
    result.y_plane.resize(static_cast<size_t>(geometry->tw) * geometry->th, 0);
    result.cb_plane.resize(static_cast<size_t>(geometry->chroma_tw) * geometry->chroma_th, max_val / 2);
    result.cr_plane.resize(static_cast<size_t>(geometry->chroma_tw) * geometry->chroma_th, max_val / 2);

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const size_t block_index = static_cast<size_t>(by) * blocks_x + bx;
            float block_scale = has_scale_streams ? y_scales[block_index] : 1.0f;
            if ((is_v9 || is_v10) && block_scale == 1.0f) {
                const uint8_t sb = superblock_scale_for(superblock_scales, blocks_x, bx, by);
                block_scale = QuantTable::kQuantScaleFactors[sb];
            }
            DctBlock recon_dct{};
            for (int i = 0; i < 64; ++i) {
                recon_dct[ZIGZAG_ORDER[i]] = static_cast<float>((*y_coeffs)[block_index][i]) * y_steps[i] * block_scale;
            }
            dct_2d_inverse(recon_dct);

            const uint32_t bx0 = bx * 8;
            const uint32_t by0 = by * 8;
            const bool has_above = by > 0;
            const bool has_left = bx > 0;
            int16_t above[8] = {};
            int16_t left[8] = {};
            int16_t above_left = max_val / 2;

            if (has_above) {
                for (int c = 0; c < 8; ++c) {
                    const uint32_t px = bx0 + c;
                    if (px < geometry->tw) {
                        above[c] = result.y_plane[(by0 - 1) * geometry->tw + px];
                    }
                }
            }
            if (has_left) {
                for (int r = 0; r < 8; ++r) {
                    const uint32_t py = by0 + r;
                    if (py < geometry->th) {
                        left[r] = result.y_plane[py * geometry->tw + (bx0 - 1)];
                    }
                }
            }
            if (has_above && has_left) {
                above_left = result.y_plane[(by0 - 1) * geometry->tw + (bx0 - 1)];
            }

            int16_t prediction[64];
            predict_8x8((*y_modes)[block_index], has_above ? above : nullptr,
                        has_left ? left : nullptr, above_left, prediction, max_val);

            for (int r = 0; r < 8; ++r) {
                for (int c = 0; c < 8; ++c) {
                    const uint32_t px = bx0 + c;
                    const uint32_t py = by0 + r;
                    if (px < geometry->tw && py < geometry->th) {
                        const int value = static_cast<int>(std::round(recon_dct[r * 8 + c])) + prediction[r * 8 + c];
                        result.y_plane[py * geometry->tw + px] = static_cast<int16_t>(
                            std::clamp(value, 0, static_cast<int>(max_val)));
                    }
                }
            }
        }
    }

    auto reconstruct_chroma = [&](const std::vector<DctBlockI16>& coeffs,
                                  const std::vector<PredMode>& modes,
                                  std::vector<int16_t>& plane) {
        const int16_t chroma_mid = static_cast<int16_t>((static_cast<int>(max_val) + 1) / 2);
        for (uint32_t by = 0; by < chroma_blocks_y; ++by) {
            for (uint32_t bx = 0; bx < chroma_blocks_x; ++bx) {
                const size_t block_index = static_cast<size_t>(by) * chroma_blocks_x + bx;
                float block_scale = has_scale_streams ? c_scales[block_index] : 1.0f;
                if ((is_v9 || is_v10) && block_scale == 1.0f) {
                    // Chroma is luma-block indexed; map to the
                    // top-left covered luma block, as in the encoder.
                    const uint32_t luma_bx =
                        subsampling == ChromaSubsampling::YUV420 ? bx * 2 : bx;
                    const uint32_t luma_by =
                        subsampling == ChromaSubsampling::YUV420 ? by * 2 : by;
                    const uint8_t sb = superblock_scale_for(
                        superblock_scales, blocks_x, luma_bx, luma_by);
                    block_scale = QuantTable::kQuantScaleFactors[sb];
                }
                DctBlock recon_dct{};
                for (int i = 0; i < 64; ++i) {
                    recon_dct[ZIGZAG_ORDER[i]] = static_cast<float>(coeffs[block_index][i]) * c_steps[i] * block_scale;
                }
                dct_2d_inverse(recon_dct);

                const uint32_t bx0 = bx * 8;
                const uint32_t by0 = by * 8;
                int16_t above[8] = {};
                int16_t left[8] = {};
                int16_t above_left = chroma_mid;
                const bool has_above = by > 0;
                const bool has_left = bx > 0;

                if (has_above) {
                    for (int c = 0; c < 8; ++c) {
                        const uint32_t px = bx0 + c;
                        if (px < geometry->chroma_tw) {
                            above[c] = plane[(by0 - 1) * geometry->chroma_tw + px];
                        }
                    }
                }
                if (has_left) {
                    for (int r = 0; r < 8; ++r) {
                        const uint32_t py = by0 + r;
                        if (py < geometry->chroma_th) {
                            left[r] = plane[py * geometry->chroma_tw + (bx0 - 1)];
                        }
                    }
                }
                if (has_above && has_left) {
                    above_left = plane[(by0 - 1) * geometry->chroma_tw + (bx0 - 1)];
                }

                int16_t prediction[64];
                const PredMode mode = modes[block_index];
                if (mode == PredMode::CCLM) {
                    // CCLM predicts chroma from the co-located
                    // reconstructed luma (already decoded, since luma
                    // precedes chroma). The (a, b) are derived from
                    // the neighbouring reconstructed luma/chroma,
                    // identically to the encoder.
                    predict_chroma_block_cclm(
                        result.y_plane.data(), geometry->tw, geometry->th,
                        plane.data(), geometry->chroma_tw, geometry->chroma_th,
                        bx, by, subsampling, prediction, max_val);
                } else {
                    predict_8x8(mode,
                                has_above ? above : nullptr,
                                has_left ? left : nullptr,
                                above_left,
                                prediction,
                                max_val);
                }

                for (int r = 0; r < 8; ++r) {
                    for (int c = 0; c < 8; ++c) {
                        const uint32_t px = bx0 + c;
                        const uint32_t py = by0 + r;
                        if (px < geometry->chroma_tw && py < geometry->chroma_th) {
                            const int value = static_cast<int>(std::round(recon_dct[r * 8 + c])) + prediction[r * 8 + c];
                            plane[py * geometry->chroma_tw + px] = static_cast<int16_t>(
                                std::clamp(value, 0, static_cast<int>(max_val)));
                        }
                    }
                }
            }
        }
    };

    reconstruct_chroma(cb_coeffs, *chroma_modes, result.cb_plane);
    reconstruct_chroma(cr_coeffs, *chroma_modes, result.cr_plane);

    if (has_alpha != tile_has_alpha) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
            has_alpha ? "lossy alpha tile is missing alpha data" : "unexpected lossy alpha tile data"});
    }

    if (has_alpha) {
        std::array<uint16_t, 64> a_steps{};
        for (int i = 0; i < 64; ++i) {
            auto value = reader.read_u16();
            if (!value) return std::unexpected(value.error());
            a_steps[i] = *value;
        }

        auto a_modes = (is_v8 || is_v9 || is_v10) ? read_rans_prediction_modes(reader, num_y_blocks, "alpha")
                             : packed_mode_streams ? read_packed_prediction_modes(reader, num_y_blocks, "alpha")
                                                   : read_raw_modes(num_y_blocks, "alpha");
        if (!a_modes) {
            return std::unexpected(a_modes.error());
        }
        if (!has_scale_streams) {
            for (PredMode mode : *a_modes) {
                if (mode == PredMode::PLANAR) {
                    return std::unexpected(Error{ErrorCode::DecodeFailed,
                                                 "planar prediction mode requires layout V7"});
                }
            }
        }

        std::vector<uint8_t> alpha_spans(num_y_blocks, 64);
        if (is_v8 || is_v9 || is_v10) {
            auto spans = read_rans_coefficient_spans(reader, num_y_blocks, "alpha");
            if (!spans) {
                return std::unexpected(spans.error());
            }
            alpha_spans = std::move(*spans);
        } else if (packed_coefficient_spans) {
            auto spans = adaptive_span_streams
                ? read_adaptive_coefficient_spans(reader, num_y_blocks, "alpha")
                : read_packed_coefficient_spans(reader, num_y_blocks, "alpha");
            if (!spans) {
                return std::unexpected(spans.error());
            }
            alpha_spans = std::move(*spans);
        }

        uint8_t alpha_max_coeff_span = 64;
        if (explicit_plane_max_coeff_spans) {
            auto alpha_max = reader.read_u8();
            if (!alpha_max) {
                return std::unexpected(Error{ErrorCode::TruncatedInput,
                                             "missing alpha coefficient extent"});
            }
            alpha_max_coeff_span = *alpha_max;
        }
        if (alpha_max_coeff_span > 64) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "alpha coefficient extent is out of range"});
        }
        for (uint8_t span_value : alpha_spans) {
            if (span_value > alpha_max_coeff_span) {
                return std::unexpected(Error{ErrorCode::DecodeFailed,
                                             "alpha coefficient span exceeds plane extent"});
            }
        }

        auto a_coeffs = decode_lossy_plane_payload(reader, alpha_spans, alpha_max_coeff_span, coeff_config);
        if (!a_coeffs) return std::unexpected(a_coeffs.error());

        result.alpha_plane.resize(static_cast<size_t>(geometry->tw) * geometry->th, max_val);
        for (uint32_t by = 0; by < blocks_y; ++by) {
            for (uint32_t bx = 0; bx < blocks_x; ++bx) {
                const size_t block_index = static_cast<size_t>(by) * blocks_x + bx;
                DctBlock recon_dct{};
                for (int i = 0; i < 64; ++i) {
                    recon_dct[ZIGZAG_ORDER[i]] = static_cast<float>((*a_coeffs)[block_index][i]) * a_steps[i];
                }
                dct_2d_inverse(recon_dct);

                const uint32_t bx0 = bx * 8;
                const uint32_t by0 = by * 8;
                const bool has_above = by > 0;
                const bool has_left = bx > 0;
                int16_t above[8] = {};
                int16_t left[8] = {};
                int16_t above_left = max_val;

                if (has_above) {
                    for (int c = 0; c < 8; ++c) {
                        const uint32_t px = bx0 + c;
                        if (px < geometry->tw) {
                            above[c] = result.alpha_plane[(by0 - 1) * geometry->tw + px];
                        }
                    }
                }
                if (has_left) {
                    for (int r = 0; r < 8; ++r) {
                        const uint32_t py = by0 + r;
                        if (py < geometry->th) {
                            left[r] = result.alpha_plane[py * geometry->tw + (bx0 - 1)];
                        }
                    }
                }
                if (has_above && has_left) {
                    above_left = result.alpha_plane[(by0 - 1) * geometry->tw + (bx0 - 1)];
                }

                int16_t prediction[64];
                predict_8x8((*a_modes)[block_index], has_above ? above : nullptr,
                            has_left ? left : nullptr, above_left, prediction, max_val);

                for (int r = 0; r < 8; ++r) {
                    for (int c = 0; c < 8; ++c) {
                        const uint32_t px = bx0 + c;
                        const uint32_t py = by0 + r;
                        if (px < geometry->tw && py < geometry->th) {
                            const int value = static_cast<int>(std::round(recon_dct[r * 8 + c])) + prediction[r * 8 + c];
                            result.alpha_plane[py * geometry->tw + px] = static_cast<int16_t>(
                                std::clamp(value, 0, static_cast<int>(max_val)));
                        }
                    }
                }
            }
        }
    }

    if (!reader.at_end()) {
        return std::unexpected(Error{ErrorCode::InvalidChunkSize, "unexpected trailing tile payload"});
    }

    return result;
}
}

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

    auto subsampling = infer_subsampling_from_tile(*first_tile_payload, first_tile_header.tile_x,
                                                   first_tile_header.tile_y, tile_size,
                                                   width, height);
    if (!subsampling) return std::unexpected(subsampling.error());

    const int16_t max_val = static_cast<int16_t>((1 << hdr.bit_depth) - 1);
    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<int16_t> full_y(pixel_count, 0);

    const uint32_t chroma_w = *subsampling == ChromaSubsampling::YUV420 ? (width + 1) / 2 : width;
    const uint32_t chroma_h = *subsampling == ChromaSubsampling::YUV420 ? (height + 1) / 2 : height;
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

    std::vector<int16_t> cb_full(pixel_count);
    std::vector<int16_t> cr_full(pixel_count);
    if (*subsampling == ChromaSubsampling::YUV420) {
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


