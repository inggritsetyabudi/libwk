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
#include "coeff_span_stream.h"
#include "container.h"
#include "dct.h"
#include "lossy_coeff_stream.h"
#include "mode_stream.h"
#include "predict.h"
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
    const bool packed_mode_streams = *layout_tag == kLossyTileLayoutTagV2 ||
                                     *layout_tag == kLossyTileLayoutTagV3 ||
                                     *layout_tag == kLossyTileLayoutTagV4 ||
                                     *layout_tag == kLossyTileLayoutTagV5 ||
                                     *layout_tag == kLossyTileLayoutTagV6;
    const bool raw_mode_streams = *layout_tag == kLossyTileLayoutTagV1;
    const bool packed_coefficient_spans = *layout_tag == kLossyTileLayoutTagV3 ||
                                          *layout_tag == kLossyTileLayoutTagV4 ||
                                          *layout_tag == kLossyTileLayoutTagV5 ||
                                          *layout_tag == kLossyTileLayoutTagV6;
    const bool adaptive_coefficient_tables = *layout_tag == kLossyTileLayoutTagV4 ||
                                             *layout_tag == kLossyTileLayoutTagV5 ||
                                             *layout_tag == kLossyTileLayoutTagV6;
    if (!packed_mode_streams && !raw_mode_streams) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unsupported lossy tile layout"});
    }

    uint8_t syntax_flags = 0;
    if (*layout_tag == kLossyTileLayoutTagV6) {
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
    const bool adaptive_span_streams = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagAdaptiveSpanStreams) != 0;
    const bool split_magnitude_signs = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagSplitMagnitudeSigns) != 0;
    const bool shared_chroma_coeff_tables = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagSharedChromaCoeffTables) != 0;
    const bool coefficient_table_bank = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagCoefficientTableBank) != 0;
    const bool elide_single_symbol_streams = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagElideSingleSymbolStreams) != 0;
    const bool coefficient_significance_maps = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagCoefficientSignificanceMaps) != 0;
    const bool adaptive_coefficient_signs = *layout_tag == kLossyTileLayoutTagV6 &&
        (syntax_flags & kLossyTileSyntaxFlagAdaptiveCoefficientSigns) != 0;
    const bool explicit_plane_max_coeff_spans = *layout_tag == kLossyTileLayoutTagV5 ||
        (*layout_tag == kLossyTileLayoutTagV6 &&
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

    auto y_modes = packed_mode_streams ? read_packed_prediction_modes(reader, num_y_blocks, "luma")
                                       : read_raw_modes(num_y_blocks, "luma");
    if (!y_modes) {
        return std::unexpected(y_modes.error());
    }

    auto chroma_modes = packed_mode_streams ? read_packed_prediction_modes(reader, num_c_blocks, "chroma")
                                            : read_raw_modes(num_c_blocks, "chroma");
    if (!chroma_modes) {
        return std::unexpected(chroma_modes.error());
    }

    std::vector<uint8_t> y_spans(num_y_blocks, 64);
    if (packed_coefficient_spans) {
        auto spans = adaptive_span_streams
            ? read_adaptive_coefficient_spans(reader, num_y_blocks, "luma")
            : read_packed_coefficient_spans(reader, num_y_blocks, "luma");
        if (!spans) {
            return std::unexpected(spans.error());
        }
        y_spans = std::move(*spans);
    }

    std::vector<uint8_t> chroma_spans(num_c_blocks, 64);
    if (packed_coefficient_spans) {
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
            DctBlock recon_dct{};
            for (int i = 0; i < 64; ++i) {
                recon_dct[ZIGZAG_ORDER[i]] = static_cast<float>((*y_coeffs)[block_index][i]) * y_steps[i];
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
                DctBlock recon_dct{};
                for (int i = 0; i < 64; ++i) {
                    recon_dct[ZIGZAG_ORDER[i]] = static_cast<float>(coeffs[block_index][i]) * c_steps[i];
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
                predict_8x8(mode,
                            has_above ? above : nullptr,
                            has_left ? left : nullptr,
                            above_left,
                            prediction,
                            max_val);

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

        auto a_modes = packed_mode_streams ? read_packed_prediction_modes(reader, num_y_blocks, "alpha")
                                           : read_raw_modes(num_y_blocks, "alpha");
        if (!a_modes) {
            return std::unexpected(a_modes.error());
        }

        std::vector<uint8_t> alpha_spans(num_y_blocks, 64);
        if (packed_coefficient_spans) {
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
        if (file->tile_chunks.empty()) {
            return std::unexpected(Error{ErrorCode::DecodeFailed, "no tile chunks"});
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


