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
#include "lossy_tile_decode.h"
#include "coeff_span_stream.h"
#include "dct.h"
#include "lossy_coeff_stream.h"
#include "mode_span_rans.h"
#include "mode_stream.h"
#include "predict.h"
#include "quantize.h"
#include "superblock_quant.h"
#include <wk/wk.hpp>
#include <cmath>

namespace wk {

Result<TileDecodeResult> decode_lossy_tile(std::span<const uint8_t> tile_data,
                                            uint16_t tile_x, uint16_t tile_y,
                                            uint32_t tile_size,
                                            uint32_t image_width, uint32_t image_height,
                                            uint8_t bit_depth,
                                            ChromaSubsampling subsampling,
                                            uint8_t layer_flags,
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
    const bool is_v11 = *layout_tag == kLossyTileLayoutTagV11;
    const bool is_v6_family = *layout_tag == kLossyTileLayoutTagV6 || is_v7 || is_v8 || is_v9 || is_v10 || is_v11;
    const bool has_scale_streams = is_v7 || is_v8 || is_v9 || is_v10 || is_v11;
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
    if (!packed_mode_streams && !raw_mode_streams && !is_v8 && !is_v9 && !is_v10 && !is_v11) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unsupported lossy tile layout"});
    }

    // V11: the tile's chroma subsampling comes from the tile header
    // flag, not from the caller (the caller only knows the image-wide
    // inference, which is meaningless for a mixed file).
    if (is_v11) {
        subsampling = (layer_flags & TILE_CHROMA_420) != 0
            ? ChromaSubsampling::YUV420 : ChromaSubsampling::YUV444;
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
    if (is_v9 || is_v10 || is_v11) {
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
    // V11: the flag is authoritative, so the block dimensions must
    // agree with it (a tile that claims 420 in the flag but carries
    // 444 block counts, or vice versa, is corrupt).
    if (is_v11 && geometry->subsampling != subsampling) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
            "V11 tile chroma block dimensions do not match the header subsampling flag"});
    }
    // The geometry (from the block counts) is authoritative for this
    // tile: the caller's subsampling is only the image-wide inference
    // from the first tile, which is wrong for a legacy 444 tile in a
    // mixed file. Sync it so the superblock mapping and CCLM below
    // use this tile's real subsampling.
    subsampling = geometry->subsampling;

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

    auto y_modes = (is_v8 || is_v9 || is_v10 || is_v11) ? read_rans_prediction_modes(reader, num_y_blocks, "luma")
                         : packed_mode_streams ? read_packed_prediction_modes(reader, num_y_blocks, "luma")
                                               : read_raw_modes(num_y_blocks, "luma");
    if (!y_modes) {
        return std::unexpected(y_modes.error());
    }

    auto chroma_modes = (is_v8 || is_v9 || is_v10 || is_v11) ? read_rans_prediction_modes(reader, num_c_blocks, "chroma")
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
        if (!is_v10 && !is_v11 && uses_cclm(*chroma_modes)) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "CCLM prediction mode requires layout V10 or V11"});
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
    if (is_v8 || is_v9 || is_v10 || is_v11) {
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
    if (is_v8 || is_v9 || is_v10 || is_v11) {
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
            if ((is_v9 || is_v10 || is_v11) && block_scale == 1.0f) {
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
                if ((is_v9 || is_v10 || is_v11) && block_scale == 1.0f) {
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

        auto a_modes = (is_v8 || is_v9 || is_v10 || is_v11) ? read_rans_prediction_modes(reader, num_y_blocks, "alpha")
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
        if (is_v8 || is_v9 || is_v10 || is_v11) {
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

}  // namespace wk
