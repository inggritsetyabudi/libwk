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
#include "control_field.h"
#include "superblock_quant.h"
#include "dct.h"
#include "lossy_coeff_stream.h"
#include "mode_span_rans.h"
#include "mode_stream.h"
#include "quantize.h"
#include "predict.h"
#include "colorspace.h"
#include "lossless.h"
#include "threading.h"
#include <wk/wk.hpp>
#include <wk/wk.h>
#include <wk/wkmeta.hpp>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <exception>
#include <new>
#include <optional>

namespace wk {

struct TileEncodeResult {
    uint16_t tile_x;
    uint16_t tile_y;
    std::vector<uint8_t> compressed;
    float    quality_score;
    TileStats stats;  // filled only when the caller passes a stats sink
};

struct BlockData {
    PredMode mode;
    DctBlockI16 quantized;
    // Adaptive quantization (layout V7): index into
    // QuantTable::kQuantScaleFactors. Identity (4) unless the tile's
    // scale policy says otherwise; the V7 scale streams carry only the
    // non-identity entries.
    uint8_t quant_scale_index = QuantTable::kQuantScaleIdentity;
};

static std::vector<DctBlockI16> collect_quantized_blocks(const std::vector<BlockData>& blocks) {
    std::vector<DctBlockI16> quantized_blocks;
    quantized_blocks.reserve(blocks.size());
    for (const auto& block : blocks) {
        quantized_blocks.push_back(block.quantized);
    }
    return quantized_blocks;
}

// Writes one V7 sparse quant-scale list: uint16 count, then
// (block_index uint16, scale_index uint8) per non-identity block, in
// raster order (strictly increasing by construction). A tile holds at
// most 1024x1024 pixels, so block indices always fit in uint16.
static void write_quant_scale_stream(ByteWriter& writer, const std::vector<BlockData>& blocks) {
    uint16_t count = 0;
    for (const auto& block : blocks) {
        if (block.quant_scale_index != QuantTable::kQuantScaleIdentity) {
            ++count;
        }
    }
    writer.write_u16(count);
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].quant_scale_index != QuantTable::kQuantScaleIdentity) {
            writer.write_u16(static_cast<uint16_t>(i));
            writer.write_u8(blocks[i].quant_scale_index);
        }
    }
}

static Result<uint8_t> resolve_tile_size_log2(const EncoderConfig& config) {
    if (config.tile_size_log2 != 0) {
        if (config.tile_size_log2 < 6 || config.tile_size_log2 > 10) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                "tile_size_log2 must be 0 or between 6 and 10"});
        }
        return config.tile_size_log2;
    }

    if (config.lossless) {
        return static_cast<uint8_t>(9);
    }

    return static_cast<uint8_t>(10);
}


static float compute_quality_score(const int16_t* original, const int16_t* reconstructed,
                                    uint32_t width, uint32_t height, int16_t max_val) {
    double mse = 0;
    size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; i++) {
        double d = static_cast<double>(original[i]) - reconstructed[i];
        mse += d * d;
    }
    mse /= count;

    if (mse < 1e-10) return 100.0f;

    double psnr = 10.0 * std::log10(
        static_cast<double>(max_val) * max_val / mse);




    float score = static_cast<float>(std::clamp((psnr - 20.0) * 2.5, 0.0, 100.0));
    return score;
}

struct TileVisualComplexity {
    float mean_chroma_deviation = 0.0f;
    float mean_chroma_gradient = 0.0f;
    float mean_luma_gradient = 0.0f;
    float max_chroma_deviation = 0.0f;
    float mean_luma = 0.0f;
    float bright_fraction = 0.0f;
    float dark_fraction = 0.0f;
    float saturated_fraction = 0.0f;
    float strong_color_edge_fraction = 0.0f;
    float highlight_edge_fraction = 0.0f;
};

static TileVisualComplexity analyze_tile_chroma_complexity(
    const int16_t* y_plane, const int16_t* cb_plane, const int16_t* cr_plane,
    uint32_t image_width, uint32_t image_height,
    uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
    ChromaSubsampling subsampling, int16_t max_val) {

    TileVisualComplexity stats;
    const float inv_max = 1.0f / std::max(1, static_cast<int>(max_val));
    const int16_t chroma_mid = static_cast<int16_t>((static_cast<int>(max_val) + 1) / 2);
    const bool is_subsampled = subsampling == ChromaSubsampling::YUV420;
    const uint32_t chroma_step = is_subsampled ? 2u : 1u;

    const uint32_t chroma_w = is_subsampled ? (image_width + 1) / 2 : image_width;
    const uint32_t chroma_h = is_subsampled ? (image_height + 1) / 2 : image_height;
    const uint32_t chroma_x0 = is_subsampled ? x0 / 2 : x0;
    const uint32_t chroma_y0 = is_subsampled ? y0 / 2 : y0;
    const uint32_t chroma_tw = is_subsampled ? (tw + 1) / 2 : tw;
    const uint32_t chroma_th = is_subsampled ? (th + 1) / 2 : th;

    double chroma_dev_sum = 0.0;
    double chroma_grad_sum = 0.0;
    size_t chroma_dev_count = 0;
    size_t chroma_grad_count = 0;
    size_t saturated_count = 0;
    size_t strong_color_edge_count = 0;

    for (uint32_t row = 0; row < chroma_th; ++row) {
        for (uint32_t col = 0; col < chroma_tw; ++col) {
            const uint32_t sx = chroma_x0 + col;
            const uint32_t sy = chroma_y0 + row;
            if (sx >= chroma_w || sy >= chroma_h) {
                continue;
            }

            const size_t idx = static_cast<size_t>(sy) * chroma_w + sx;
            const int cb = cb_plane[idx];
            const int cr = cr_plane[idx];
            const float deviation = 0.5f * (std::abs(cb - chroma_mid) + std::abs(cr - chroma_mid)) * inv_max;
            chroma_dev_sum += deviation;
            stats.max_chroma_deviation = std::max(stats.max_chroma_deviation, deviation);
            if (deviation > 0.18f) {
                ++saturated_count;
            }
            ++chroma_dev_count;

            float local_chroma_edge = 0.0f;
            if (col + 1 < chroma_tw && sx + 1 < chroma_w) {
                const size_t right_idx = idx + 1;
                const float chroma_diff = static_cast<float>(0.5 * (
                    std::abs(cb - static_cast<int>(cb_plane[right_idx])) +
                    std::abs(cr - static_cast<int>(cr_plane[right_idx])))) * inv_max;
                chroma_grad_sum += chroma_diff;
                local_chroma_edge = std::max(local_chroma_edge, chroma_diff);
                ++chroma_grad_count;
            }
            if (row + 1 < chroma_th && sy + 1 < chroma_h) {
                const size_t down_idx = idx + chroma_w;
                const float chroma_diff = static_cast<float>(0.5 * (
                    std::abs(cb - static_cast<int>(cb_plane[down_idx])) +
                    std::abs(cr - static_cast<int>(cr_plane[down_idx])))) * inv_max;
                chroma_grad_sum += chroma_diff;
                local_chroma_edge = std::max(local_chroma_edge, chroma_diff);
                ++chroma_grad_count;
            }

            const uint32_t luma_x = std::min(x0 + col * chroma_step, image_width - 1);
            const uint32_t luma_y = std::min(y0 + row * chroma_step, image_height - 1);
            const size_t luma_idx = static_cast<size_t>(luma_y) * image_width + luma_x;
            float local_luma_edge = 0.0f;
            if (luma_x + chroma_step < image_width && luma_x + chroma_step < x0 + tw) {
                local_luma_edge = std::max(local_luma_edge,
                    std::abs(y_plane[luma_idx] - static_cast<int>(y_plane[luma_idx + chroma_step])) * inv_max);
            }
            if (luma_y + chroma_step < image_height && luma_y + chroma_step < y0 + th) {
                local_luma_edge = std::max(local_luma_edge,
                    std::abs(y_plane[luma_idx] - static_cast<int>(y_plane[luma_idx + static_cast<size_t>(chroma_step) * image_width])) * inv_max);
            }

            if (deviation > 0.14f && local_luma_edge > 0.08f && local_chroma_edge > 0.04f) {
                ++strong_color_edge_count;
            }
        }
    }

    double luma_sum = 0.0;
    double luma_grad_sum = 0.0;
    size_t luma_count = 0;
    size_t luma_grad_count = 0;
    size_t bright_count = 0;
    size_t dark_count = 0;
    size_t highlight_edge_count = 0;

    for (uint32_t row = 0; row < th; ++row) {
        for (uint32_t col = 0; col < tw; ++col) {
            const size_t idx = static_cast<size_t>(y0 + row) * image_width + (x0 + col);
            const int y = y_plane[idx];
            const float normalized_luma = static_cast<float>(y) * inv_max;
            luma_sum += normalized_luma;
            ++luma_count;
            if (normalized_luma >= 0.72f) {
                ++bright_count;
            }
            if (normalized_luma <= 0.18f) {
                ++dark_count;
            }

            float local_luma_edge = 0.0f;
            if (col + 1 < tw) {
                const float diff = std::abs(y - static_cast<int>(y_plane[idx + 1])) * inv_max;
                luma_grad_sum += diff;
                local_luma_edge = std::max(local_luma_edge, diff);
                ++luma_grad_count;
            }
            if (row + 1 < th) {
                const float diff = std::abs(y - static_cast<int>(y_plane[idx + image_width])) * inv_max;
                luma_grad_sum += diff;
                local_luma_edge = std::max(local_luma_edge, diff);
                ++luma_grad_count;
            }
            if (normalized_luma >= 0.62f && local_luma_edge > 0.08f) {
                ++highlight_edge_count;
            }
        }
    }

    if (chroma_dev_count > 0) {
        stats.mean_chroma_deviation = static_cast<float>(chroma_dev_sum / chroma_dev_count);
        stats.saturated_fraction = static_cast<float>(saturated_count) / chroma_dev_count;
        stats.strong_color_edge_fraction = static_cast<float>(strong_color_edge_count) / chroma_dev_count;
    }
    if (chroma_grad_count > 0) {
        stats.mean_chroma_gradient = static_cast<float>(chroma_grad_sum / chroma_grad_count);
    }
    if (luma_count > 0) {
        stats.mean_luma = static_cast<float>(luma_sum / luma_count);
        stats.bright_fraction = static_cast<float>(bright_count) / luma_count;
        stats.dark_fraction = static_cast<float>(dark_count) / luma_count;
        stats.highlight_edge_fraction = static_cast<float>(highlight_edge_count) / luma_count;
    }
    if (luma_grad_count > 0) {
        stats.mean_luma_gradient = static_cast<float>(luma_grad_sum / luma_grad_count);
    }

    return stats;
}

static float adaptive_chroma_quality_for_tile(float base_quality,
                                              const TileVisualComplexity& stats,
                                              ChromaSubsampling subsampling) {
    const float detail_score = std::clamp(
        stats.mean_chroma_deviation * 0.85f +
        stats.max_chroma_deviation * 0.22f +
        stats.mean_chroma_gradient * 1.20f +
        stats.mean_luma_gradient * 0.55f +
        stats.highlight_edge_fraction * 0.40f,
        0.0f, 1.5f);

    const float protection_score = std::clamp(
        stats.mean_chroma_deviation * 0.70f +
        stats.max_chroma_deviation * 0.60f +
        stats.mean_chroma_gradient * 1.00f +
        stats.mean_luma_gradient * 0.40f +
        stats.saturated_fraction * 0.90f +
        stats.strong_color_edge_fraction * 1.10f +
        stats.bright_fraction * 0.55f +
        stats.highlight_edge_fraction * 1.20f,
        0.0f, 2.0f);

    float delta = 0.0f;
    if (detail_score > 0.78f) {
        delta = std::max(delta, 2.5f);
    } else if (detail_score > 0.60f) {
        delta = std::max(delta, 1.5f);
    } else if (detail_score > 0.42f) {
        delta = std::max(delta, 0.75f);
    }

    if (protection_score > 1.05f) {
        delta = std::max(delta, 8.0f);
    } else if (protection_score > 0.82f) {
        delta = std::max(delta, 6.0f);
    } else if (protection_score > 0.60f) {
        delta = std::max(delta, 4.0f);
    } else if (protection_score > 0.42f) {
        delta = std::max(delta, 2.5f);
    } else if (protection_score > 0.30f) {
        delta = std::max(delta, 1.0f);
    }

    if (stats.max_chroma_deviation > 0.32f && stats.mean_luma_gradient > 0.09f) {
        delta = std::max(delta, 4.0f);
    }
    if (stats.saturated_fraction > 0.12f && stats.mean_chroma_gradient > 0.06f) {
        delta = std::max(delta, 3.0f);
    }
    if (stats.strong_color_edge_fraction > 0.05f) {
        delta = std::max(delta, 3.0f);
    }
    if (stats.bright_fraction > 0.30f && stats.mean_chroma_deviation > 0.18f) {
        delta = std::max(delta, 4.5f);
    }
    if (stats.dark_fraction > 0.22f && stats.mean_chroma_deviation > 0.05f &&
        stats.mean_chroma_gradient > 0.02f && stats.mean_luma_gradient > 0.04f) {
        delta = std::max(delta, 2.5f);
    }
    if (stats.mean_chroma_deviation < 0.16f && stats.mean_chroma_gradient > 0.03f &&
        stats.mean_luma_gradient > 0.05f) {
        delta = std::max(delta, 1.75f);
    }
    if (stats.highlight_edge_fraction > 0.05f && stats.mean_chroma_gradient > 0.04f) {
        delta = std::max(delta, 3.5f);
    }

    if (subsampling == ChromaSubsampling::YUV420 && delta > 0.0f) {
        delta *= 1.15f;
    }

    return std::clamp(base_quality + delta, 1.0f, 100.0f);
}

static float adaptive_luma_quality_for_tile(float base_quality,
                                            const TileVisualComplexity& stats,
                                            ChromaSubsampling subsampling) {
    const float highlight_score = std::clamp(
        stats.mean_luma * 0.40f +
        stats.bright_fraction * 0.95f +
        stats.mean_luma_gradient * 1.25f +
        stats.highlight_edge_fraction * 1.10f +
        stats.strong_color_edge_fraction * 0.75f,
        0.0f, 1.8f);

    float delta = 0.0f;
    if (highlight_score > 1.15f) {
        delta = std::max(delta, 4.0f);
    } else if (highlight_score > 0.88f) {
        delta = std::max(delta, 3.0f);
    } else if (highlight_score > 0.68f) {
        delta = std::max(delta, 2.0f);
    } else if (highlight_score > 0.50f) {
        delta = std::max(delta, 1.0f);
    }

    if (stats.bright_fraction > 0.34f && stats.mean_luma_gradient > 0.06f) {
        delta = std::max(delta, 3.5f);
    }
    if (stats.highlight_edge_fraction > 0.08f) {
        delta = std::max(delta, 2.5f);
    }
    if (stats.strong_color_edge_fraction > 0.06f && stats.mean_luma_gradient > 0.05f) {
        delta = std::max(delta, 2.0f);
    }

    if (subsampling == ChromaSubsampling::YUV420 && delta > 0.0f) {
        delta *= 1.10f;
    }

    return std::clamp(base_quality + delta, 1.0f, 100.0f);
}

// (clamp_quantized_to_stream_range lives in lossy_coeff_stream.h,
// next to coeff_symbol_offset, so the RDO can use it too.)

static std::vector<uint8_t> expand_rgb8_to_rgba8(const Image& image) {
    std::vector<uint8_t> expanded(static_cast<size_t>(image.width()) * image.height() * 4, 255);
    const auto pixels = image.pixels();
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    for (size_t i = 0; i < pixel_count; ++i) {
        expanded[i * 4 + 0] = pixels[i * 3 + 0];
        expanded[i * 4 + 1] = pixels[i * 3 + 1];
        expanded[i * 4 + 2] = pixels[i * 3 + 2];
    }
    return expanded;
}

// A tile is a smooth chroma ramp when its chroma spans a wide range
// (mean_deviation) yet changes only gradually (both gradients small) with
// no strong color edges. Only such tiles get the finer low-frequency
// chroma steps: on flat chroma the refinement is pure waste, and on
// textured content it costs bytes and can flip RDO modes for the worse.
// Thresholds measured against the 6 benchmark photos plus synthetic
// gradient/gray/solid tiles -- the ramp is the only tile that passes all
// five, with ~2-3x margin on each.
static bool tile_has_smooth_chroma_ramp(const TileVisualComplexity& stats) {
    return stats.mean_chroma_deviation > 0.10f &&
           stats.max_chroma_deviation > 0.15f &&
           stats.mean_chroma_gradient < 0.003f &&
           stats.mean_luma_gradient < 0.003f &&
           stats.strong_color_edge_fraction < 0.001f;
}

static Result<TileEncodeResult> encode_lossy_tile(
    const int16_t* y_plane, const int16_t* cb_plane, const int16_t* cr_plane,
    const int16_t* alpha_plane,
    uint32_t image_width, uint32_t image_height,
    uint32_t tile_x, uint32_t tile_y, uint32_t tile_size,
    float quality, uint8_t bit_depth, bool has_alpha,
    ChromaSubsampling subsampling,
    bool use_control_field,
    TileStats* stats = nullptr) {

    if (has_alpha && alpha_plane == nullptr) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "alpha plane missing"});
    }

    int16_t max_val = static_cast<int16_t>((1 << bit_depth) - 1);

    uint32_t x0 = tile_x * tile_size;
    uint32_t y0 = tile_y * tile_size;
    uint32_t tw = std::min(tile_size, image_width - x0);
    uint32_t th = std::min(tile_size, image_height - y0);

    const TileVisualComplexity tile_stats = analyze_tile_chroma_complexity(
        y_plane, cb_plane, cr_plane,
        image_width, image_height,
        x0, y0, tw, th,
        subsampling, max_val);

    float luma_quality = adaptive_luma_quality_for_tile(quality, tile_stats, subsampling);
    float chroma_quality = quality;
    if (subsampling == ChromaSubsampling::YUV444) {
        float chroma_boost = 8.0f;
        if (quality >= 90.0f) {
            chroma_boost = 3.0f;
        } else if (quality >= 85.0f) {
            chroma_boost = 5.0f;
        } else if (quality >= 75.0f) {
            chroma_boost = 6.0f;
        }
        chroma_quality = std::min(100.0f, quality + chroma_boost);
    } else {
        chroma_quality = std::min(100.0f, quality + 4.0f);
    }
    chroma_quality = adaptive_chroma_quality_for_tile(chroma_quality, tile_stats, subsampling);

    const float max_luma_boost = quality >= 85.0f ? 2.5f : 3.0f;
    const float max_chroma_boost = subsampling == ChromaSubsampling::YUV444
        ? (quality >= 85.0f ? 7.0f : 8.5f)
        : (quality >= 85.0f ? 8.0f : 10.0f);
    luma_quality = std::min(luma_quality, quality + max_luma_boost);
    chroma_quality = std::min(chroma_quality, quality + max_chroma_boost);

    int qp = quality_to_qp(luma_quality);
    float lambda = qp_to_lambda(qp);
    const int chroma_qp = quality_to_qp(chroma_quality);
    const float chroma_lambda = qp_to_lambda(chroma_qp);

    uint32_t blocks_x = (tw + 7) / 8;
    uint32_t blocks_y = (th + 7) / 8;

    std::vector<BlockData> y_blocks(blocks_x * blocks_y);
    std::vector<BlockData> cb_blocks, cr_blocks, a_blocks;

    uint32_t chroma_tw = tw;
    uint32_t chroma_th = th;
    uint32_t chroma_blocks_x = blocks_x;
    uint32_t chroma_blocks_y = blocks_y;
    if (subsampling == ChromaSubsampling::YUV420) {
        chroma_tw = (tw + 1) / 2;
        chroma_th = (th + 1) / 2;
        chroma_blocks_x = (chroma_tw + 7) / 8;
        chroma_blocks_y = (chroma_th + 7) / 8;
    }
    cb_blocks.resize(chroma_blocks_x * chroma_blocks_y);
    cr_blocks.resize(chroma_blocks_x * chroma_blocks_y);
    if (has_alpha) {
        a_blocks.resize(blocks_x * blocks_y);
    }

    std::vector<int16_t> tile_y_data(tw * th, 0);
    for (uint32_t r = 0; r < th; ++r) {
        for (uint32_t c = 0; c < tw; ++c) {
            tile_y_data[r * tw + c] = y_plane[(y0 + r) * image_width + (x0 + c)];
        }
    }

    // Phase 2 control field: per-block content analysis of this tile's
    // luma. Phase 3 aggregates it to a per-superblock quant map (V9);
    // the per-block V7 scales proved too expensive to signal for a
    // dense field (see superblock_quant.h), so the field's per-block
    // scale is NOT used directly — only its superblock aggregate is.
    const std::vector<BlockControl> control_field =
        use_control_field ? analyze_control_field(tile_y_data.data(), tw, th)
                          : std::vector<BlockControl>{};
    const std::vector<uint8_t> superblock_scales =
        use_control_field
            ? aggregate_superblock_scales(control_field, blocks_x, blocks_y)
            : std::vector<uint8_t>{};
    const bool use_v9_superblock_map =
        use_control_field && superblock_map_is_nontrivial(superblock_scales);

    // The smooth-ramp gate (fix #2) still drives the chroma table
    // refinement. NOTE: a seed boost (first row/column at quant scale
    // 0.5 via layout V7) was tried here and REVERTED 2026-11-13: it did
    // not fix the q50 ramp (distinct 82 -> 83, worst step 4 -> 5, size
    // +27%; luma-only was worse at +73%) because the block-boundary
    // steps come from the within-block flatness concentrating the
    // ramp's rise at boundaries, not from the seed row's step size --
    // and the finer steps flip the RDO layout choices chaotically.
    // V7 stays as the adaptive-quantization foundation (SPEC 3.2);
    // no policy currently emits a non-identity scale, so all tiles
    // are V6 until a policy that actually helps lands.
    const bool smooth_ramp_tile = tile_has_smooth_chroma_ramp(tile_stats);
    // Seed boost, retried under the quantization-aware RDO: on a
    // smooth-ramp tile at low quality, the seed blocks (first row or
    // first column, which have no above/left reference) get quant
    // scale 0.75. The seed is the bottleneck -- the first row's block
    // means sit on a 2-level grid (DC step 16) and every interior
    // block copies the row above, extruding the seed's staircase down
    // the tile. Scaling the seed's step by 0.75 puts its means on a
    // finer grid. (An earlier 0.5 scale was measured and rejected.)
    // This failed under the OLD RDO (which could not value the
    // finer step); the new RDO measures the quantized cost, so it is
    // retried here. The per-block scale is threaded into the RDO
    // calls so the mode decision and the quantize agree.
    const bool seed_boost_tile = smooth_ramp_tile && quality < 75.0f;
    // NOTE: a V7 per-block scale of 0.5 for smooth-ramp tiles at q<75
    // was tried under the quantization-aware RDO and REVERTED
    // 2026-11-13: it raised q50 distinct only 80 -> 85 while the
    // worst step got worse (4 -> 5) and the file grew +275%
    // (2637 -> 9883 B). The RDO's bit estimate undercounts the true
    // rANS cost of the finer coefficients, so the finer step is not
    // worth it. V7 stays dormant (no policy emits a non-identity
    // scale) until a policy with a better size/quality tradeoff lands.

    QuantTable quant_y, quant_c, quant_a;
    quant_y.build(luma_quality, false, bit_depth);
    quant_c.build(chroma_quality, true, bit_depth, smooth_ramp_tile);
    if (has_alpha) {
        quant_a.build(std::min(100.0f, quality + 10.0f), false, bit_depth);
    }

    std::vector<int16_t> reconstructed_y(tw * th, 0);

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            int16_t original_block[64] = {};
            uint32_t bx0 = bx * 8;
            uint32_t by0 = by * 8;

            for (int r = 0; r < 8; ++r) {
                for (int c = 0; c < 8; ++c) {
                    uint32_t px = bx0 + c;
                    uint32_t py = by0 + r;
                    if (px < tw && py < th) {
                        original_block[r * 8 + c] = tile_y_data[py * tw + px];
                    }
                }
            }

            int16_t above[8] = {};
            int16_t left[8] = {};
            int16_t above_left = max_val / 2;
            bool has_above = by > 0;
            bool has_left = bx > 0;

            if (has_above) {
                for (int c = 0; c < 8; ++c) {
                    uint32_t px = bx0 + c;
                    if (px < tw) {
                        above[c] = reconstructed_y[(by0 - 1) * tw + px];
                    }
                }
            }
            if (has_left) {
                for (int r = 0; r < 8; ++r) {
                    uint32_t py = by0 + r;
                    if (py < th) {
                        left[r] = reconstructed_y[py * tw + (bx0 - 1)];
                    }
                }
            }
            if (has_above && has_left) {
                above_left = reconstructed_y[(by0 - 1) * tw + (bx0 - 1)];
            }

            // Per-block scale (seed boost only). This is what
            // BlockData stores and what the V7/V8 per-block stream
            // serializes — it stays sparse and cheap. The superblock
            // scale (V9 map) is applied on top at quantize time but
            // is NOT stored per block; the decoder reconstructs it
            // from the map. Effective scale: per-block wins when
            // non-identity, else the block's superblock scale.
            const uint8_t per_block_scale_index =
                (seed_boost_tile && (by == 0 || bx == 0))
                    ? 2 : QuantTable::kQuantScaleIdentity;
            const uint8_t sb_scale_index =
                superblock_scale_for(superblock_scales, blocks_x, bx, by);
            const uint8_t block_scale_index =
                per_block_scale_index != QuantTable::kQuantScaleIdentity
                    ? per_block_scale_index : sb_scale_index;
            const float block_scale =
                QuantTable::kQuantScaleFactors[block_scale_index];
            auto rd = select_best_mode(original_block,
                                        has_above ? above : nullptr,
                                        has_left ? left : nullptr,
                                        above_left, lambda, max_val,
                                        quant_y, bit_depth, block_scale);

            int16_t pred[64];
            predict_8x8(rd.mode, has_above ? above : nullptr,
                        has_left ? left : nullptr, above_left, pred, max_val);

            int16_t residual_block[64];
            for (int i = 0; i < 64; ++i) {
                residual_block[i] = original_block[i] - pred[i];
            }

            DctBlock dct_coeffs;
            for (int i = 0; i < 64; ++i) {
                dct_coeffs[i] = static_cast<float>(residual_block[i]);
            }
            dct_2d_forward(dct_coeffs);

            auto& bd = y_blocks[by * blocks_x + bx];
            bd.mode = rd.mode;
            bd.quant_scale_index = per_block_scale_index;
            quant_y.quantize_scaled(dct_coeffs, bd.quantized, block_scale);
            clamp_quantized_to_stream_range(bd.quantized, bit_depth);

            DctBlock recon_dct;
            quant_y.dequantize_scaled(bd.quantized, recon_dct, block_scale);
            dct_2d_inverse(recon_dct);

            for (int r = 0; r < 8; ++r) {
                for (int c = 0; c < 8; ++c) {
                    uint32_t px = bx0 + c;
                    uint32_t py = by0 + r;
                    if (px < tw && py < th) {
                        int16_t v = static_cast<int16_t>(
                            std::clamp(static_cast<int>(std::round(recon_dct[r * 8 + c]))
                                       + pred[r * 8 + c],
                                       0, static_cast<int>(max_val)));
                        reconstructed_y[py * tw + px] = v;
                    }
                }
            }
        }
    }

    const int16_t chroma_mid = static_cast<int16_t>((static_cast<int>(max_val) + 1) / 2);
    std::vector<int16_t> tile_cb_data(chroma_tw * chroma_th, chroma_mid);
    std::vector<int16_t> tile_cr_data(chroma_tw * chroma_th, chroma_mid);
    std::vector<int16_t> reconstructed_cb(chroma_tw * chroma_th, chroma_mid);
    std::vector<int16_t> reconstructed_cr(chroma_tw * chroma_th, chroma_mid);

    const uint32_t chroma_src_w = subsampling == ChromaSubsampling::YUV420 ? (image_width + 1) / 2 : image_width;
    const uint32_t chroma_src_h = subsampling == ChromaSubsampling::YUV420 ? (image_height + 1) / 2 : image_height;
    const uint32_t chroma_x0 = subsampling == ChromaSubsampling::YUV420 ? x0 / 2 : x0;
    const uint32_t chroma_y0 = subsampling == ChromaSubsampling::YUV420 ? y0 / 2 : y0;

    for (uint32_t r = 0; r < chroma_th; ++r) {
        for (uint32_t c = 0; c < chroma_tw; ++c) {
            const uint32_t sy = chroma_y0 + r;
            const uint32_t sx = chroma_x0 + c;
            if (sy < chroma_src_h && sx < chroma_src_w) {
                tile_cb_data[r * chroma_tw + c] = cb_plane[sy * chroma_src_w + sx];
                tile_cr_data[r * chroma_tw + c] = cr_plane[sy * chroma_src_w + sx];
            }
        }
    }

    auto select_joint_chroma_mode = [&](const int16_t* cb_original,
                                        const int16_t* cb_above,
                                        const int16_t* cb_left,
                                        int16_t cb_above_left,
                                        const int16_t* cr_original,
                                        const int16_t* cr_above,
                                        const int16_t* cr_left,
                                        int16_t cr_above_left,
                                        float quant_scale = 1.0f) {
        RdResult best{PredMode::DC, 1.0e30f};
        int16_t cb_prediction[64];
        int16_t cr_prediction[64];

        // Quantization-aware, like select_best_mode: each plane's
        // residual is transformed, quantized, clamped, dequantized and
        // added back, so the cost uses the reconstructed block and the
        // quantized coefficients -- what the decoder and the stream
        // will actually see.
        auto plane_rd = [&](const int16_t* original, const int16_t* prediction,
                            float& ssd_out, float& bits_out) {
            int16_t residual[64];
            int16_t reconstructed[64];
            for (int i = 0; i < 64; ++i) {
                residual[i] = original[i] - prediction[i];
            }
            DctBlock dct_coeffs;
            for (int i = 0; i < 64; ++i) {
                dct_coeffs[i] = static_cast<float>(residual[i]);
            }
            dct_2d_forward(dct_coeffs);
            DctBlockI16 quantized;
            quant_c.quantize_scaled(dct_coeffs, quantized, quant_scale);
            clamp_quantized_to_stream_range(quantized, bit_depth);
            DctBlock recon_dct;
            quant_c.dequantize_scaled(quantized, recon_dct, quant_scale);
            dct_2d_inverse(recon_dct);
            for (int i = 0; i < 64; ++i) {
                const int value = static_cast<int>(std::lround(recon_dct[i])) + prediction[i];
                reconstructed[i] = static_cast<int16_t>(std::clamp(value, 0, static_cast<int>(max_val)));
            }
            ssd_out = compute_ssd_8x8(original, reconstructed);
            bits_out = estimate_bits_8x8(quantized.data());
        };

        for (int mode_index = 0; mode_index < static_cast<int>(PredMode::NUM_MODES); ++mode_index) {
            const PredMode mode = static_cast<PredMode>(mode_index);
            predict_8x8(mode, cb_above, cb_left, cb_above_left, cb_prediction, max_val);
            predict_8x8(mode, cr_above, cr_left, cr_above_left, cr_prediction, max_val);

            float cb_ssd = 0, cb_bits = 0, cr_ssd = 0, cr_bits = 0;
            plane_rd(cb_original, cb_prediction, cb_ssd, cb_bits);
            plane_rd(cr_original, cr_prediction, cr_ssd, cr_bits);

            const float distortion = cb_ssd + cr_ssd;
            const float bits = cb_bits + cr_bits;
            const float cost = distortion + chroma_lambda * bits;
            if (cost < best.cost) {
                best.mode = mode;
                best.cost = cost;
            }
        }

        return best.mode;
    };

    auto encode_chroma_component = [&](const int16_t* original_block,
                                       const int16_t* prediction,
                                       BlockData& block_data,
                                       std::vector<int16_t>& reconstructed_plane,
                                       uint32_t bx0,
                                       uint32_t by0,
                                       float quant_scale) {
        int16_t residual_block[64];
        for (int i = 0; i < 64; ++i) {
            residual_block[i] = original_block[i] - prediction[i];
        }

        DctBlock dct_coeffs;
        for (int i = 0; i < 64; ++i) {
            dct_coeffs[i] = static_cast<float>(residual_block[i]);
        }
        dct_2d_forward(dct_coeffs);
        // quant_scale is the EFFECTIVE scale (per-block or superblock);
        // block_data.quant_scale_index stores only the per-block part
        // for the V7/V8 stream, so it is not read here.
        quant_c.quantize_scaled(dct_coeffs, block_data.quantized, quant_scale);
        clamp_quantized_to_stream_range(block_data.quantized, bit_depth);

        DctBlock recon_dct;
        quant_c.dequantize_scaled(block_data.quantized, recon_dct, quant_scale);
        dct_2d_inverse(recon_dct);

        for (int r = 0; r < 8; ++r) {
            for (int c = 0; c < 8; ++c) {
                const uint32_t px = bx0 + c;
                const uint32_t py = by0 + r;
                if (px < chroma_tw && py < chroma_th) {
                    const int value = static_cast<int>(std::round(recon_dct[r * 8 + c])) + prediction[r * 8 + c];
                    reconstructed_plane[py * chroma_tw + px] = static_cast<int16_t>(
                        std::clamp(value, 0, static_cast<int>(max_val)));
                }
            }
        }
    };

    for (uint32_t by = 0; by < chroma_blocks_y; ++by) {
        for (uint32_t bx = 0; bx < chroma_blocks_x; ++bx) {
            int16_t original_cb_block[64] = {};
            int16_t original_cr_block[64] = {};
            const uint32_t bx0 = bx * 8;
            const uint32_t by0 = by * 8;

            for (int r = 0; r < 8; ++r) {
                for (int c = 0; c < 8; ++c) {
                    const uint32_t px = bx0 + c;
                    const uint32_t py = by0 + r;
                    if (px < chroma_tw && py < chroma_th) {
                        original_cb_block[r * 8 + c] = tile_cb_data[py * chroma_tw + px];
                        original_cr_block[r * 8 + c] = tile_cr_data[py * chroma_tw + px];
                    }
                }
            }

            int16_t cb_above[8] = {};
            int16_t cb_left[8] = {};
            int16_t cr_above[8] = {};
            int16_t cr_left[8] = {};
            int16_t cb_above_left = chroma_mid;
            int16_t cr_above_left = chroma_mid;
            const bool has_above = by > 0;
            const bool has_left = bx > 0;

            if (has_above) {
                for (int c = 0; c < 8; ++c) {
                    const uint32_t px = bx0 + c;
                    if (px < chroma_tw) {
                        cb_above[c] = reconstructed_cb[(by0 - 1) * chroma_tw + px];
                        cr_above[c] = reconstructed_cr[(by0 - 1) * chroma_tw + px];
                    }
                }
            }
            if (has_left) {
                for (int r = 0; r < 8; ++r) {
                    const uint32_t py = by0 + r;
                    if (py < chroma_th) {
                        cb_left[r] = reconstructed_cb[py * chroma_tw + (bx0 - 1)];
                        cr_left[r] = reconstructed_cr[py * chroma_tw + (bx0 - 1)];
                    }
                }
            }
            if (has_above && has_left) {
                cb_above_left = reconstructed_cb[(by0 - 1) * chroma_tw + (bx0 - 1)];
                cr_above_left = reconstructed_cr[(by0 - 1) * chroma_tw + (bx0 - 1)];
            }

            // The control field is luma-block indexed. In YUV444 the
            // chroma block (bx,by) is the same block; in YUV420 it
            // covers luma blocks (2bx,2by)..(2bx+1,2by+1), so use the
            // top-left one as representative.
            const uint32_t luma_bx =
                subsampling == ChromaSubsampling::YUV420 ? bx * 2 : bx;
            const uint32_t luma_by =
                subsampling == ChromaSubsampling::YUV420 ? by * 2 : by;
            // Per-block scale (seed boost only) for the stream; the
            // effective scale (per-block or superblock) for quantize.
            const uint8_t c_per_block_scale_index =
                (seed_boost_tile && (by == 0 || bx == 0))
                    ? 2 : QuantTable::kQuantScaleIdentity;
            const uint8_t c_sb_scale_index =
                superblock_scale_for(superblock_scales, blocks_x, luma_bx, luma_by);
            const uint8_t c_block_scale_index =
                c_per_block_scale_index != QuantTable::kQuantScaleIdentity
                    ? c_per_block_scale_index : c_sb_scale_index;
            const float c_block_scale =
                QuantTable::kQuantScaleFactors[c_block_scale_index];
            const PredMode chroma_mode = select_joint_chroma_mode(
                original_cb_block,
                has_above ? cb_above : nullptr,
                has_left ? cb_left : nullptr,
                cb_above_left,
                original_cr_block,
                has_above ? cr_above : nullptr,
                has_left ? cr_left : nullptr,
                cr_above_left,
                c_block_scale);

            int16_t cb_prediction[64];
            int16_t cr_prediction[64];
            predict_8x8(chroma_mode, has_above ? cb_above : nullptr,
                        has_left ? cb_left : nullptr, cb_above_left, cb_prediction, max_val);
            predict_8x8(chroma_mode, has_above ? cr_above : nullptr,
                        has_left ? cr_left : nullptr, cr_above_left, cr_prediction, max_val);

            auto& cb_block = cb_blocks[by * chroma_blocks_x + bx];
            auto& cr_block = cr_blocks[by * chroma_blocks_x + bx];
            cb_block.mode = chroma_mode;
            cr_block.mode = chroma_mode;
            cb_block.quant_scale_index = c_per_block_scale_index;
            cr_block.quant_scale_index = c_per_block_scale_index;

            encode_chroma_component(original_cb_block, cb_prediction, cb_block, reconstructed_cb, bx0, by0, c_block_scale);
            encode_chroma_component(original_cr_block, cr_prediction, cr_block, reconstructed_cr, bx0, by0, c_block_scale);
        }
    }

    std::vector<int16_t> tile_alpha_data;
    std::vector<int16_t> reconstructed_alpha;
    if (has_alpha) {
        tile_alpha_data.assign(tw * th, max_val);
        reconstructed_alpha.assign(tw * th, max_val);
        for (uint32_t r = 0; r < th; ++r) {
            for (uint32_t c = 0; c < tw; ++c) {
                tile_alpha_data[r * tw + c] = alpha_plane[(y0 + r) * image_width + (x0 + c)];
            }
        }

        for (uint32_t by = 0; by < blocks_y; ++by) {
            for (uint32_t bx = 0; bx < blocks_x; ++bx) {
                int16_t original_block[64] = {};
                uint32_t bx0 = bx * 8;
                uint32_t by0 = by * 8;

                for (int r = 0; r < 8; ++r) {
                    for (int c = 0; c < 8; ++c) {
                        uint32_t px = bx0 + c;
                        uint32_t py = by0 + r;
                        if (px < tw && py < th) {
                            original_block[r * 8 + c] = tile_alpha_data[py * tw + px];
                        }
                    }
                }

                int16_t above[8] = {};
                int16_t left[8] = {};
                int16_t above_left = max_val;
                bool has_above = by > 0;
                bool has_left = bx > 0;

                if (has_above) {
                    for (int c = 0; c < 8; ++c) {
                        uint32_t px = bx0 + c;
                        if (px < tw) {
                            above[c] = reconstructed_alpha[(by0 - 1) * tw + px];
                        }
                    }
                }
                if (has_left) {
                    for (int r = 0; r < 8; ++r) {
                        uint32_t py = by0 + r;
                        if (py < th) {
                            left[r] = reconstructed_alpha[py * tw + (bx0 - 1)];
                        }
                    }
                }
                if (has_above && has_left) {
                    above_left = reconstructed_alpha[(by0 - 1) * tw + (bx0 - 1)];
                }

                auto rd = select_best_mode(original_block,
                                            has_above ? above : nullptr,
                                            has_left ? left : nullptr,
                                            above_left, lambda, max_val,
                                            quant_a, bit_depth);

                int16_t pred[64];
                predict_8x8(rd.mode, has_above ? above : nullptr,
                            has_left ? left : nullptr, above_left, pred, max_val);

                int16_t residual_block[64];
                for (int i = 0; i < 64; ++i) {
                    residual_block[i] = original_block[i] - pred[i];
                }

                DctBlock dct_coeffs;
                for (int i = 0; i < 64; ++i) {
                    dct_coeffs[i] = static_cast<float>(residual_block[i]);
                }
                dct_2d_forward(dct_coeffs);

                auto& bd = a_blocks[by * blocks_x + bx];
                bd.mode = rd.mode;
                quant_a.quantize(dct_coeffs, bd.quantized);
                clamp_quantized_to_stream_range(bd.quantized, bit_depth);

                DctBlock recon_dct;
                quant_a.dequantize(bd.quantized, recon_dct);
                dct_2d_inverse(recon_dct);

                for (int r = 0; r < 8; ++r) {
                    for (int c = 0; c < 8; ++c) {
                        uint32_t px = bx0 + c;
                        uint32_t py = by0 + r;
                        if (px < tw && py < th) {
                            int16_t v = static_cast<int16_t>(
                                std::clamp(static_cast<int>(std::round(recon_dct[r * 8 + c]))
                                           + pred[r * 8 + c],
                                           0, static_cast<int>(max_val)));
                            reconstructed_alpha[py * tw + px] = v;
                        }
                    }
                }
            }
        }
    }

    ByteWriter tile_writer;

    // TileStats checkpoints (Phase 1 diagnostic): record the writer size
    // at each section boundary, so the breakdown is exact.
    size_t mark = 0;
    auto mark_bytes = [&]() {
        const size_t now = tile_writer.size();
        const size_t delta = now - mark;
        mark = now;
        return delta;
    };

    for (int i = 0; i < 64; ++i) tile_writer.write_u16(quant_y.step(i));
    for (int i = 0; i < 64; ++i) tile_writer.write_u16(quant_c.step(i));
    const size_t quant_table_bytes = mark_bytes();

    tile_writer.write_u16(static_cast<uint16_t>(blocks_x));
    tile_writer.write_u16(static_cast<uint16_t>(blocks_y));
    tile_writer.write_u16(static_cast<uint16_t>(chroma_blocks_x));
    tile_writer.write_u16(static_cast<uint16_t>(chroma_blocks_y));
    const size_t block_dims_bytes = mark_bytes();

    std::vector<uint8_t> y_spans;
    y_spans.reserve(y_blocks.size());
    for (const auto& block : y_blocks) {
        y_spans.push_back(compute_coefficient_span(block.quantized));
    }

    std::vector<uint8_t> chroma_spans;
    chroma_spans.reserve(cb_blocks.size());
    for (size_t i = 0; i < cb_blocks.size(); ++i) {
        const uint8_t cb_span = compute_coefficient_span(cb_blocks[i].quantized);
        const uint8_t cr_span = compute_coefficient_span(cr_blocks[i].quantized);
        chroma_spans.push_back(std::max(cb_span, cr_span));
    }

    std::vector<uint8_t> alpha_spans;
    if (has_alpha) {
        alpha_spans.reserve(a_blocks.size());
        for (const auto& block : a_blocks) {
            alpha_spans.push_back(compute_coefficient_span(block.quantized));
        }
    }

    const uint8_t y_max_coeff_span = y_spans.empty()
        ? 0
        : *std::max_element(y_spans.begin(), y_spans.end());
    const uint8_t chroma_max_coeff_span = chroma_spans.empty()
        ? 0
        : *std::max_element(chroma_spans.begin(), chroma_spans.end());
    const uint8_t alpha_max_coeff_span = alpha_spans.empty()
        ? 0
        : *std::max_element(alpha_spans.begin(), alpha_spans.end());
    const size_t plane_extent_overhead = 2u + (has_alpha ? 1u : 0u);
    const size_t estimated_plane_extent_savings =
        static_cast<size_t>(64 - y_max_coeff_span) * 7u +
        static_cast<size_t>(64 - chroma_max_coeff_span) * 14u +
        static_cast<size_t>(64 - alpha_max_coeff_span) * (has_alpha ? 7u : 0u);
    const bool use_plane_max_coeff_spans = estimated_plane_extent_savings > plane_extent_overhead;
    const size_t legacy_span_stream_bytes =
        sizeof(uint16_t) + packed_coefficient_span_bytes(y_spans.size()) +
        sizeof(uint16_t) + packed_coefficient_span_bytes(chroma_spans.size()) +
        (has_alpha ? sizeof(uint16_t) + packed_coefficient_span_bytes(alpha_spans.size()) : 0u);
    auto adaptive_y_span_bytes = adaptive_coefficient_span_stream_bytes(y_spans);
    if (!adaptive_y_span_bytes) {
        return std::unexpected(adaptive_y_span_bytes.error());
    }
    auto adaptive_chroma_span_bytes = adaptive_coefficient_span_stream_bytes(chroma_spans);
    if (!adaptive_chroma_span_bytes) {
        return std::unexpected(adaptive_chroma_span_bytes.error());
    }
    size_t adaptive_alpha_span_bytes = 0;
    if (has_alpha) {
        auto encoded_alpha_span_bytes = adaptive_coefficient_span_stream_bytes(alpha_spans);
        if (!encoded_alpha_span_bytes) {
            return std::unexpected(encoded_alpha_span_bytes.error());
        }
        adaptive_alpha_span_bytes = *encoded_alpha_span_bytes;
    }
    const size_t adaptive_span_stream_bytes =
        *adaptive_y_span_bytes + *adaptive_chroma_span_bytes +
        adaptive_alpha_span_bytes;
    const size_t adaptive_span_layout_overhead = 1u;
    const bool use_adaptive_span_streams =
        adaptive_span_stream_bytes + adaptive_span_layout_overhead < legacy_span_stream_bytes;
    const auto y_quantized_blocks = collect_quantized_blocks(y_blocks);
    const auto cb_quantized_blocks = collect_quantized_blocks(cb_blocks);
    const auto cr_quantized_blocks = collect_quantized_blocks(cr_blocks);
    const auto a_quantized_blocks = has_alpha ? collect_quantized_blocks(a_blocks) : std::vector<DctBlockI16>{};

    const LossyCoeffStreamConfig legacy_coeff_config{
        .use_plane_max_coeff_span = use_plane_max_coeff_spans,
        .split_magnitude_signs = false,
        .bit_depth = bit_depth,
    };
    const LossyCoeffStreamConfig split_coeff_config{
        .use_plane_max_coeff_span = use_plane_max_coeff_spans,
        .split_magnitude_signs = true,
        .bit_depth = bit_depth,
    };

    auto y_legacy_coeff_payload = encode_lossy_plane_payload(y_quantized_blocks, y_spans, y_max_coeff_span, legacy_coeff_config);
    if (!y_legacy_coeff_payload) {
        return std::unexpected(y_legacy_coeff_payload.error());
    }
    auto cb_legacy_coeff_payload = encode_lossy_plane_payload(cb_quantized_blocks, chroma_spans, chroma_max_coeff_span, legacy_coeff_config);
    if (!cb_legacy_coeff_payload) {
        return std::unexpected(cb_legacy_coeff_payload.error());
    }
    auto cr_legacy_coeff_payload = encode_lossy_plane_payload(cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, legacy_coeff_config);
    if (!cr_legacy_coeff_payload) {
        return std::unexpected(cr_legacy_coeff_payload.error());
    }
    Result<std::vector<uint8_t>> alpha_legacy_coeff_payload = std::vector<uint8_t>{};
    if (has_alpha) {
        alpha_legacy_coeff_payload = encode_lossy_plane_payload(a_quantized_blocks, alpha_spans, alpha_max_coeff_span, legacy_coeff_config);
        if (!alpha_legacy_coeff_payload) {
            return std::unexpected(alpha_legacy_coeff_payload.error());
        }
    }

    auto y_split_coeff_payload = encode_lossy_plane_payload(y_quantized_blocks, y_spans, y_max_coeff_span, split_coeff_config);
    if (!y_split_coeff_payload) {
        return std::unexpected(y_split_coeff_payload.error());
    }
    auto cb_split_coeff_payload = encode_lossy_plane_payload(cb_quantized_blocks, chroma_spans, chroma_max_coeff_span, split_coeff_config);
    if (!cb_split_coeff_payload) {
        return std::unexpected(cb_split_coeff_payload.error());
    }
    auto cr_split_coeff_payload = encode_lossy_plane_payload(cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, split_coeff_config);
    if (!cr_split_coeff_payload) {
        return std::unexpected(cr_split_coeff_payload.error());
    }
    Result<std::vector<uint8_t>> alpha_split_coeff_payload = std::vector<uint8_t>{};
    if (has_alpha) {
        alpha_split_coeff_payload = encode_lossy_plane_payload(a_quantized_blocks, alpha_spans, alpha_max_coeff_span, split_coeff_config);
        if (!alpha_split_coeff_payload) {
            return std::unexpected(alpha_split_coeff_payload.error());
        }
    }

    const size_t legacy_coeff_payload_bytes =
        y_legacy_coeff_payload->size() +
        cb_legacy_coeff_payload->size() +
        cr_legacy_coeff_payload->size() +
        (has_alpha ? alpha_legacy_coeff_payload->size() : 0u);
    const size_t split_coeff_payload_bytes =
        y_split_coeff_payload->size() +
        cb_split_coeff_payload->size() +
        cr_split_coeff_payload->size() +
        (has_alpha ? alpha_split_coeff_payload->size() : 0u);
    const size_t split_sign_layout_overhead = use_adaptive_span_streams ? 0u : 1u;
    const bool use_split_magnitude_signs =
        split_coeff_payload_bytes + split_sign_layout_overhead < legacy_coeff_payload_bytes;

    const auto& selected_y_coeff_payload = use_split_magnitude_signs ? *y_split_coeff_payload
                                                                     : *y_legacy_coeff_payload;
    const auto& selected_alpha_coeff_payload = use_split_magnitude_signs ? *alpha_split_coeff_payload
                                                                         : *alpha_legacy_coeff_payload;
    const LossyCoeffStreamConfig selected_coeff_config = use_split_magnitude_signs
        ? split_coeff_config
        : legacy_coeff_config;

    auto shared_legacy_chroma_payload = encode_lossy_chroma_payload(
        cb_quantized_blocks, cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, legacy_coeff_config);
    if (!shared_legacy_chroma_payload) {
        return std::unexpected(shared_legacy_chroma_payload.error());
    }
    auto shared_split_chroma_payload = encode_lossy_chroma_payload(
        cb_quantized_blocks, cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, split_coeff_config);
    if (!shared_split_chroma_payload) {
        return std::unexpected(shared_split_chroma_payload.error());
    }
    const auto& selected_independent_cb_payload = use_split_magnitude_signs ? *cb_split_coeff_payload
                                                                            : *cb_legacy_coeff_payload;
    const auto& selected_independent_cr_payload = use_split_magnitude_signs ? *cr_split_coeff_payload
                                                                            : *cr_legacy_coeff_payload;
    const auto& selected_shared_chroma_payload = use_split_magnitude_signs ? *shared_split_chroma_payload
                                                                           : *shared_legacy_chroma_payload;
    const size_t independent_chroma_payload_bytes =
        selected_independent_cb_payload.size() + selected_independent_cr_payload.size();
    const size_t shared_chroma_layout_overhead =
        (use_adaptive_span_streams || use_split_magnitude_signs) ? 0u : 1u;
    const bool base_use_shared_chroma_coeff_tables =
        selected_shared_chroma_payload.size() + shared_chroma_layout_overhead < independent_chroma_payload_bytes;
    const size_t base_chroma_coeff_payload_bytes = base_use_shared_chroma_coeff_tables
        ? selected_shared_chroma_payload.size()
        : independent_chroma_payload_bytes;
    const size_t base_lossy_syntax_bytes =
        (use_adaptive_span_streams || use_split_magnitude_signs || base_use_shared_chroma_coeff_tables) ? 1u : 0u;
    const size_t base_coeff_payload_bytes =
        selected_y_coeff_payload.size() +
        base_chroma_coeff_payload_bytes +
        (has_alpha ? selected_alpha_coeff_payload.size() : 0u) +
        base_lossy_syntax_bytes;

    LossyCoeffStreamConfig bank_coeff_config = selected_coeff_config;
    bank_coeff_config.use_table_bank = true;
    bank_coeff_config.use_table_cluster_selection = true;

    auto y_banked_coeff_payload = encode_lossy_plane_payload(y_quantized_blocks, y_spans, y_max_coeff_span, bank_coeff_config);
    if (!y_banked_coeff_payload) {
        return std::unexpected(y_banked_coeff_payload.error());
    }
    auto cb_banked_coeff_payload = encode_lossy_plane_payload(cb_quantized_blocks, chroma_spans, chroma_max_coeff_span, bank_coeff_config);
    if (!cb_banked_coeff_payload) {
        return std::unexpected(cb_banked_coeff_payload.error());
    }
    auto cr_banked_coeff_payload = encode_lossy_plane_payload(cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, bank_coeff_config);
    if (!cr_banked_coeff_payload) {
        return std::unexpected(cr_banked_coeff_payload.error());
    }
    Result<std::vector<uint8_t>> alpha_banked_coeff_payload = std::vector<uint8_t>{};
    if (has_alpha) {
        alpha_banked_coeff_payload = encode_lossy_plane_payload(a_quantized_blocks, alpha_spans, alpha_max_coeff_span, bank_coeff_config);
        if (!alpha_banked_coeff_payload) {
            return std::unexpected(alpha_banked_coeff_payload.error());
        }
    }
    auto shared_banked_chroma_payload = encode_lossy_chroma_payload(
        cb_quantized_blocks, cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, bank_coeff_config);
    if (!shared_banked_chroma_payload) {
        return std::unexpected(shared_banked_chroma_payload.error());
    }

    const bool banked_use_shared_chroma_coeff_tables =
        shared_banked_chroma_payload->size() <
        cb_banked_coeff_payload->size() + cr_banked_coeff_payload->size();
    const size_t banked_chroma_coeff_payload_bytes = banked_use_shared_chroma_coeff_tables
        ? shared_banked_chroma_payload->size()
        : (cb_banked_coeff_payload->size() + cr_banked_coeff_payload->size());
    const size_t banked_coeff_payload_bytes =
        y_banked_coeff_payload->size() +
        banked_chroma_coeff_payload_bytes +
        (has_alpha ? alpha_banked_coeff_payload->size() : 0u) +
        1u;
    const bool use_coefficient_table_bank = banked_coeff_payload_bytes < base_coeff_payload_bytes;
    const size_t selected_coeff_payload_bytes = use_coefficient_table_bank
        ? banked_coeff_payload_bytes
        : base_coeff_payload_bytes;

    LossyCoeffStreamConfig elided_coeff_config = selected_coeff_config;
    elided_coeff_config.use_table_bank = use_coefficient_table_bank;
    elided_coeff_config.use_table_cluster_selection = use_coefficient_table_bank;
    elided_coeff_config.elide_single_symbol_streams = true;

    auto y_elided_coeff_payload = encode_lossy_plane_payload(y_quantized_blocks, y_spans, y_max_coeff_span, elided_coeff_config);
    if (!y_elided_coeff_payload) {
        return std::unexpected(y_elided_coeff_payload.error());
    }
    auto cb_elided_coeff_payload = encode_lossy_plane_payload(cb_quantized_blocks, chroma_spans, chroma_max_coeff_span, elided_coeff_config);
    if (!cb_elided_coeff_payload) {
        return std::unexpected(cb_elided_coeff_payload.error());
    }
    auto cr_elided_coeff_payload = encode_lossy_plane_payload(cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, elided_coeff_config);
    if (!cr_elided_coeff_payload) {
        return std::unexpected(cr_elided_coeff_payload.error());
    }
    Result<std::vector<uint8_t>> alpha_elided_coeff_payload = std::vector<uint8_t>{};
    if (has_alpha) {
        alpha_elided_coeff_payload = encode_lossy_plane_payload(a_quantized_blocks, alpha_spans, alpha_max_coeff_span, elided_coeff_config);
        if (!alpha_elided_coeff_payload) {
            return std::unexpected(alpha_elided_coeff_payload.error());
        }
    }
    auto shared_elided_chroma_payload = encode_lossy_chroma_payload(
        cb_quantized_blocks, cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, elided_coeff_config);
    if (!shared_elided_chroma_payload) {
        return std::unexpected(shared_elided_chroma_payload.error());
    }
    const bool elided_use_shared_chroma_coeff_tables =
        shared_elided_chroma_payload->size() <
        cb_elided_coeff_payload->size() + cr_elided_coeff_payload->size();
    const size_t elided_chroma_coeff_payload_bytes = elided_use_shared_chroma_coeff_tables
        ? shared_elided_chroma_payload->size()
        : (cb_elided_coeff_payload->size() + cr_elided_coeff_payload->size());
    const size_t elided_coeff_payload_bytes =
        y_elided_coeff_payload->size() +
        elided_chroma_coeff_payload_bytes +
        (has_alpha ? alpha_elided_coeff_payload->size() : 0u) +
        1u;
    const bool use_elide_single_symbol_streams =
        elided_coeff_payload_bytes < selected_coeff_payload_bytes;

    const bool current_use_shared_chroma_coeff_tables = use_elide_single_symbol_streams
        ? elided_use_shared_chroma_coeff_tables
        : (use_coefficient_table_bank ? banked_use_shared_chroma_coeff_tables
                                      : base_use_shared_chroma_coeff_tables);
    const auto& current_y_coeff_payload = use_elide_single_symbol_streams
        ? *y_elided_coeff_payload
        : (use_coefficient_table_bank ? *y_banked_coeff_payload
                                      : selected_y_coeff_payload);
    const auto& current_alpha_coeff_payload = use_elide_single_symbol_streams
        ? *alpha_elided_coeff_payload
        : (use_coefficient_table_bank ? *alpha_banked_coeff_payload
                                      : selected_alpha_coeff_payload);
    const auto& current_independent_cb_payload = use_elide_single_symbol_streams
        ? *cb_elided_coeff_payload
        : (use_coefficient_table_bank ? *cb_banked_coeff_payload
                                      : selected_independent_cb_payload);
    const auto& current_independent_cr_payload = use_elide_single_symbol_streams
        ? *cr_elided_coeff_payload
        : (use_coefficient_table_bank ? *cr_banked_coeff_payload
                                      : selected_independent_cr_payload);
    const auto& current_shared_chroma_payload = use_elide_single_symbol_streams
        ? *shared_elided_chroma_payload
        : (use_coefficient_table_bank ? *shared_banked_chroma_payload
                                      : selected_shared_chroma_payload);
    const size_t current_coeff_payload_bytes = use_elide_single_symbol_streams
        ? elided_coeff_payload_bytes
        : selected_coeff_payload_bytes;

    LossyCoeffStreamConfig significance_coeff_config = selected_coeff_config;
    significance_coeff_config.use_table_bank = use_coefficient_table_bank;
    significance_coeff_config.use_table_cluster_selection = use_coefficient_table_bank;
    significance_coeff_config.elide_single_symbol_streams = use_elide_single_symbol_streams;
    significance_coeff_config.use_significance_maps = true;

    auto y_significance_coeff_payload = encode_lossy_plane_payload(
        y_quantized_blocks, y_spans, y_max_coeff_span, significance_coeff_config);
    if (!y_significance_coeff_payload) {
        return std::unexpected(y_significance_coeff_payload.error());
    }
    auto cb_significance_coeff_payload = encode_lossy_plane_payload(
        cb_quantized_blocks, chroma_spans, chroma_max_coeff_span, significance_coeff_config);
    if (!cb_significance_coeff_payload) {
        return std::unexpected(cb_significance_coeff_payload.error());
    }
    auto cr_significance_coeff_payload = encode_lossy_plane_payload(
        cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, significance_coeff_config);
    if (!cr_significance_coeff_payload) {
        return std::unexpected(cr_significance_coeff_payload.error());
    }
    Result<std::vector<uint8_t>> alpha_significance_coeff_payload = std::vector<uint8_t>{};
    if (has_alpha) {
        alpha_significance_coeff_payload = encode_lossy_plane_payload(
            a_quantized_blocks, alpha_spans, alpha_max_coeff_span, significance_coeff_config);
        if (!alpha_significance_coeff_payload) {
            return std::unexpected(alpha_significance_coeff_payload.error());
        }
    }
    auto shared_significance_chroma_payload = encode_lossy_chroma_payload(
        cb_quantized_blocks, cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, significance_coeff_config);
    if (!shared_significance_chroma_payload) {
        return std::unexpected(shared_significance_chroma_payload.error());
    }
    const bool significance_use_shared_chroma_coeff_tables =
        shared_significance_chroma_payload->size() <
        cb_significance_coeff_payload->size() + cr_significance_coeff_payload->size();
    const size_t significance_chroma_coeff_payload_bytes = significance_use_shared_chroma_coeff_tables
        ? shared_significance_chroma_payload->size()
        : (cb_significance_coeff_payload->size() + cr_significance_coeff_payload->size());
    const size_t significance_coeff_payload_bytes =
        y_significance_coeff_payload->size() +
        significance_chroma_coeff_payload_bytes +
        (has_alpha ? alpha_significance_coeff_payload->size() : 0u) +
        1u;
    const bool use_coefficient_significance_maps =
        significance_coeff_payload_bytes < current_coeff_payload_bytes;

    const bool use_shared_chroma_coeff_tables = use_coefficient_significance_maps
        ? significance_use_shared_chroma_coeff_tables
        : current_use_shared_chroma_coeff_tables;
    const auto& final_y_coeff_payload = use_coefficient_significance_maps
        ? *y_significance_coeff_payload
        : current_y_coeff_payload;
    const auto& final_alpha_coeff_payload = use_coefficient_significance_maps
        ? *alpha_significance_coeff_payload
        : current_alpha_coeff_payload;
    const auto& final_independent_cb_payload = use_coefficient_significance_maps
        ? *cb_significance_coeff_payload
        : current_independent_cb_payload;
    const auto& final_independent_cr_payload = use_coefficient_significance_maps
        ? *cr_significance_coeff_payload
        : current_independent_cr_payload;
    const auto& final_shared_chroma_payload = use_coefficient_significance_maps
        ? *shared_significance_chroma_payload
        : current_shared_chroma_payload;
    const size_t significance_selected_coeff_payload_bytes = use_coefficient_significance_maps
        ? significance_coeff_payload_bytes
        : current_coeff_payload_bytes;

    bool use_adaptive_coefficient_signs = false;
    Result<std::vector<uint8_t>> y_sign_mode_coeff_payload = std::vector<uint8_t>{};
    Result<std::vector<uint8_t>> cb_sign_mode_coeff_payload = std::vector<uint8_t>{};
    Result<std::vector<uint8_t>> cr_sign_mode_coeff_payload = std::vector<uint8_t>{};
    Result<std::vector<uint8_t>> alpha_sign_mode_coeff_payload = std::vector<uint8_t>{};
    Result<std::vector<uint8_t>> shared_sign_mode_chroma_payload = std::vector<uint8_t>{};
    bool sign_modes_use_shared_chroma_coeff_tables = use_shared_chroma_coeff_tables;

    if (use_split_magnitude_signs) {
        LossyCoeffStreamConfig sign_mode_coeff_config = selected_coeff_config;
        sign_mode_coeff_config.use_table_bank = use_coefficient_table_bank;
        sign_mode_coeff_config.use_table_cluster_selection = use_coefficient_table_bank;
        sign_mode_coeff_config.elide_single_symbol_streams = use_elide_single_symbol_streams;
        sign_mode_coeff_config.use_significance_maps = use_coefficient_significance_maps;
        sign_mode_coeff_config.use_adaptive_sign_streams = true;

        y_sign_mode_coeff_payload = encode_lossy_plane_payload(
            y_quantized_blocks, y_spans, y_max_coeff_span, sign_mode_coeff_config);
        if (!y_sign_mode_coeff_payload) {
            return std::unexpected(y_sign_mode_coeff_payload.error());
        }
        cb_sign_mode_coeff_payload = encode_lossy_plane_payload(
            cb_quantized_blocks, chroma_spans, chroma_max_coeff_span, sign_mode_coeff_config);
        if (!cb_sign_mode_coeff_payload) {
            return std::unexpected(cb_sign_mode_coeff_payload.error());
        }
        cr_sign_mode_coeff_payload = encode_lossy_plane_payload(
            cr_quantized_blocks, chroma_spans, chroma_max_coeff_span, sign_mode_coeff_config);
        if (!cr_sign_mode_coeff_payload) {
            return std::unexpected(cr_sign_mode_coeff_payload.error());
        }
        if (has_alpha) {
            alpha_sign_mode_coeff_payload = encode_lossy_plane_payload(
                a_quantized_blocks, alpha_spans, alpha_max_coeff_span, sign_mode_coeff_config);
            if (!alpha_sign_mode_coeff_payload) {
                return std::unexpected(alpha_sign_mode_coeff_payload.error());
            }
        }
        shared_sign_mode_chroma_payload = encode_lossy_chroma_payload(
            cb_quantized_blocks, cr_quantized_blocks, chroma_spans, chroma_max_coeff_span,
            sign_mode_coeff_config);
        if (!shared_sign_mode_chroma_payload) {
            return std::unexpected(shared_sign_mode_chroma_payload.error());
        }

        sign_modes_use_shared_chroma_coeff_tables =
            shared_sign_mode_chroma_payload->size() <
            cb_sign_mode_coeff_payload->size() + cr_sign_mode_coeff_payload->size();
        const size_t sign_mode_chroma_coeff_payload_bytes = sign_modes_use_shared_chroma_coeff_tables
            ? shared_sign_mode_chroma_payload->size()
            : (cb_sign_mode_coeff_payload->size() + cr_sign_mode_coeff_payload->size());
        const size_t sign_mode_coeff_payload_bytes =
            y_sign_mode_coeff_payload->size() +
            sign_mode_chroma_coeff_payload_bytes +
            (has_alpha ? alpha_sign_mode_coeff_payload->size() : 0u) +
            1u;
        use_adaptive_coefficient_signs =
            sign_mode_coeff_payload_bytes < significance_selected_coeff_payload_bytes;
    }

    const bool final_use_shared_chroma_coeff_tables = use_adaptive_coefficient_signs
        ? sign_modes_use_shared_chroma_coeff_tables
        : use_shared_chroma_coeff_tables;
    const auto& final_y_payload = use_adaptive_coefficient_signs
        ? *y_sign_mode_coeff_payload
        : final_y_coeff_payload;
    const auto& final_alpha_payload = use_adaptive_coefficient_signs
        ? *alpha_sign_mode_coeff_payload
        : final_alpha_coeff_payload;
    const auto& final_independent_cb_coeff_payload = use_adaptive_coefficient_signs
        ? *cb_sign_mode_coeff_payload
        : final_independent_cb_payload;
    const auto& final_independent_cr_coeff_payload = use_adaptive_coefficient_signs
        ? *cr_sign_mode_coeff_payload
        : final_independent_cr_payload;
    const auto& final_shared_chroma_coeff_payload = use_adaptive_coefficient_signs
        ? *shared_sign_mode_chroma_payload
        : final_shared_chroma_payload;

    // Layout V7 (adaptive quantization): emitted when any luma or chroma
    // block uses a non-identity quant scale. The scale streams follow
    // the syntax-flags byte, before the mode streams; the chroma stream
    // is written once (Cb and Cr share one scale per block position).
    // V7 is also required when any block uses the PLANAR prediction
    // mode (13): the decoder rejects planar in V1-V6.
    auto uses_non_identity_scale = [](const std::vector<BlockData>& blocks) {
        for (const auto& block : blocks) {
            if (block.quant_scale_index != QuantTable::kQuantScaleIdentity) {
                return true;
            }
        }
        return false;
    };
    auto uses_planar_mode = [](const std::vector<BlockData>& blocks) {
        for (const auto& block : blocks) {
            if (block.mode == PredMode::PLANAR) {
                return true;
            }
        }
        return false;
    };
    const bool use_v7 = uses_non_identity_scale(y_blocks) ||
                        uses_non_identity_scale(cb_blocks) ||
                        uses_non_identity_scale(cr_blocks) ||
                        uses_planar_mode(y_blocks) ||
                        uses_planar_mode(cb_blocks) ||
                        uses_planar_mode(cr_blocks) ||
                        (has_alpha && uses_planar_mode(a_blocks));

    // Layout V8 (Phase 1): rANS-coded modes and spans. Encode both the
    // packed and the rANS variants into temp buffers and keep the smaller
    // one. The rANS table overhead is included in the rANS size, so the
    // race is honest. Modes and spans are decided together because they
    // share the layout tag.
    auto collect_modes = [](const std::vector<BlockData>& blocks) {
        std::vector<PredMode> modes;
        modes.reserve(blocks.size());
        for (const auto& block : blocks) {
            modes.push_back(block.mode);
        }
        return modes;
    };
    const auto y_modes = collect_modes(y_blocks);
    const auto chroma_modes = collect_modes(cb_blocks);
    const auto alpha_modes = has_alpha ? collect_modes(a_blocks) : std::vector<PredMode>{};

    auto rans_stream_bytes = [](auto&& write_fn) -> Result<size_t> {
        ByteWriter w;
        auto r = write_fn(w);
        if (!r) {
            return std::unexpected(r.error());
        }
        return w.finish().size();
    };
    // Packed mode bytes: u16 length + packed nibbles (see
    // write_packed_prediction_modes).
    const size_t packed_mode_bytes =
        (sizeof(uint16_t) + packed_prediction_mode_bytes(y_modes.size())) +
        (sizeof(uint16_t) + packed_prediction_mode_bytes(chroma_modes.size())) +
        (has_alpha ? sizeof(uint16_t) + packed_prediction_mode_bytes(alpha_modes.size()) : 0u);
    auto rans_y_mode_bytes = rans_stream_bytes(
        [&](ByteWriter& w) { return write_rans_prediction_modes(w, y_modes); });
    if (!rans_y_mode_bytes) {
        return std::unexpected(rans_y_mode_bytes.error());
    }
    auto rans_chroma_mode_bytes = rans_stream_bytes(
        [&](ByteWriter& w) { return write_rans_prediction_modes(w, chroma_modes); });
    if (!rans_chroma_mode_bytes) {
        return std::unexpected(rans_chroma_mode_bytes.error());
    }
    size_t rans_alpha_mode_bytes = 0;
    if (has_alpha) {
        auto r = rans_stream_bytes(
            [&](ByteWriter& w) { return write_rans_prediction_modes(w, alpha_modes); });
        if (!r) {
            return std::unexpected(r.error());
        }
        rans_alpha_mode_bytes = *r;
    }
    // Current span bytes (the adaptive/packed choice already made above).
    // Adaptive: per-plane adaptive sizes. Packed: legacy_span_stream_bytes
    // already covers Y + chroma + alpha.
    const size_t current_span_bytes = use_adaptive_span_streams
        ? (*adaptive_y_span_bytes + *adaptive_chroma_span_bytes +
           (has_alpha ? adaptive_alpha_span_bytes : 0u))
        : legacy_span_stream_bytes;
    auto rans_y_span_bytes = rans_stream_bytes(
        [&](ByteWriter& w) { return write_rans_coefficient_spans(w, y_spans); });
    if (!rans_y_span_bytes) {
        return std::unexpected(rans_y_span_bytes.error());
    }
    auto rans_chroma_span_bytes = rans_stream_bytes(
        [&](ByteWriter& w) { return write_rans_coefficient_spans(w, chroma_spans); });
    if (!rans_chroma_span_bytes) {
        return std::unexpected(rans_chroma_span_bytes.error());
    }
    size_t rans_alpha_span_bytes = 0;
    if (has_alpha) {
        auto r = rans_stream_bytes(
            [&](ByteWriter& w) { return write_rans_coefficient_spans(w, alpha_spans); });
        if (!r) {
            return std::unexpected(r.error());
        }
        rans_alpha_span_bytes = *r;
    }
    const size_t rans_mode_span_bytes = *rans_y_mode_bytes + *rans_chroma_mode_bytes +
                                        rans_alpha_mode_bytes + *rans_y_span_bytes +
                                        *rans_chroma_span_bytes + rans_alpha_span_bytes;
    // V9 implies rANS modes/spans (it is V8 plus the superblock map),
    // so a tile that uses the superblock map always takes the rANS
    // path even when packed would have been marginally smaller.
    const bool use_rans_modes_spans =
        use_v9_superblock_map ||
        rans_mode_span_bytes < packed_mode_bytes + current_span_bytes;

    uint8_t syntax_flags = 0;
    if (use_adaptive_span_streams) {
        syntax_flags |= kLossyTileSyntaxFlagAdaptiveSpanStreams;
    }
    if (use_plane_max_coeff_spans) {
        syntax_flags |= kLossyTileSyntaxFlagPlaneCoeffExtents;
    }
    if (use_split_magnitude_signs) {
        syntax_flags |= kLossyTileSyntaxFlagSplitMagnitudeSigns;
    }
    if (final_use_shared_chroma_coeff_tables) {
        syntax_flags |= kLossyTileSyntaxFlagSharedChromaCoeffTables;
    }
    if (use_coefficient_table_bank) {
        syntax_flags |= kLossyTileSyntaxFlagCoefficientTableBank;
    }
    if (use_elide_single_symbol_streams) {
        syntax_flags |= kLossyTileSyntaxFlagElideSingleSymbolStreams;
    }
    if (use_coefficient_significance_maps) {
        syntax_flags |= kLossyTileSyntaxFlagCoefficientSignificanceMaps;
    }
    if (use_adaptive_coefficient_signs) {
        syntax_flags |= kLossyTileSyntaxFlagAdaptiveCoefficientSigns;
    }

    // TileStats checkpoints: layout tag + flags (+ block dims) and the
    // V7 scale streams, measured as writer-size deltas.
    size_t layout_bytes = 0;
    size_t v7_scale_bytes = 0;
    if (use_v9_superblock_map) {
        // Layout V9: like V8 (flags byte + scale streams + rANS modes
        // and spans), plus the superblock quant map after the scale
        // streams. The map is sparse and tiny (one entry per
        // non-identity superblock); the per-block streams stay sparse
        // because they only carry the seed boost, never the
        // superblock-driven scales.
        tile_writer.write_u32(kLossyTileLayoutTagV9);
        tile_writer.write_u8(syntax_flags);
        layout_bytes = block_dims_bytes + mark_bytes();
        write_quant_scale_stream(tile_writer, y_blocks);
        write_quant_scale_stream(tile_writer, cb_blocks);
        v7_scale_bytes = mark_bytes();
        auto map_result = write_superblock_quant_map(tile_writer, superblock_scales);
        if (!map_result) {
            return std::unexpected(map_result.error());
        }
    } else if (use_rans_modes_spans) {
        // Layout V8: like V7 (flags byte + scale streams), but modes and
        // spans are rANS-coded. The scale streams are written even when
        // use_v7 is false; they are empty (count 0) then.
        tile_writer.write_u32(kLossyTileLayoutTagV8);
        tile_writer.write_u8(syntax_flags);
        layout_bytes = block_dims_bytes + mark_bytes();
        write_quant_scale_stream(tile_writer, y_blocks);
        write_quant_scale_stream(tile_writer, cb_blocks);
        v7_scale_bytes = mark_bytes();
    } else if (use_v7) {
        tile_writer.write_u32(kLossyTileLayoutTagV7);
        tile_writer.write_u8(syntax_flags);
        layout_bytes = block_dims_bytes + mark_bytes();
        write_quant_scale_stream(tile_writer, y_blocks);
        write_quant_scale_stream(tile_writer, cb_blocks);
        v7_scale_bytes = mark_bytes();
    } else if (use_adaptive_span_streams || use_split_magnitude_signs || final_use_shared_chroma_coeff_tables ||
        use_coefficient_table_bank || use_elide_single_symbol_streams ||
        use_coefficient_significance_maps || use_adaptive_coefficient_signs) {
        tile_writer.write_u32(kLossyTileLayoutTagV6);
        tile_writer.write_u8(syntax_flags);
        layout_bytes = block_dims_bytes + mark_bytes();
    } else if (use_plane_max_coeff_spans) {
        tile_writer.write_u32(kLossyTileLayoutTagV5);
        layout_bytes = block_dims_bytes + mark_bytes();
    } else {
        tile_writer.write_u32(kLossyTileLayoutTagV4);
        layout_bytes = block_dims_bytes + mark_bytes();
    }

    auto write_block_modes = [&](const std::vector<BlockData>& blocks) -> Result<void> {
        std::vector<PredMode> modes;
        modes.reserve(blocks.size());
        for (const auto& block : blocks) {
            modes.push_back(block.mode);
        }
        if (use_rans_modes_spans) {
            return write_rans_prediction_modes(tile_writer, modes);
        }
        return write_packed_prediction_modes(tile_writer, modes);
    };

    auto y_mode_result = write_block_modes(y_blocks);
    if (!y_mode_result) {
        return std::unexpected(y_mode_result.error());
    }

    auto chroma_mode_result = write_block_modes(cb_blocks);
    if (!chroma_mode_result) {
        return std::unexpected(chroma_mode_result.error());
    }
    const size_t mode_bytes = mark_bytes();

    auto write_spans = [&](std::span<const uint8_t> spans) -> Result<void> {
        if (use_rans_modes_spans) {
            return write_rans_coefficient_spans(tile_writer, spans);
        }
        return use_adaptive_span_streams
            ? write_adaptive_coefficient_spans(tile_writer, spans)
            : write_packed_coefficient_spans(tile_writer, spans);
    };

    auto y_span_result = write_spans(y_spans);
    if (!y_span_result) {
        return std::unexpected(y_span_result.error());
    }

    auto chroma_span_result = write_spans(chroma_spans);
    if (!chroma_span_result) {
        return std::unexpected(chroma_span_result.error());
    }
    const size_t span_bytes = mark_bytes();

    if (use_plane_max_coeff_spans) {
        tile_writer.write_u8(y_max_coeff_span);
        tile_writer.write_u8(chroma_max_coeff_span);
    }
    const size_t max_span_bytes = mark_bytes();

    tile_writer.write_bytes(final_y_payload);
    if (final_use_shared_chroma_coeff_tables) {
        tile_writer.write_bytes(final_shared_chroma_coeff_payload);
    } else {
        tile_writer.write_bytes(final_independent_cb_coeff_payload);
        tile_writer.write_bytes(final_independent_cr_coeff_payload);
    }

    if (has_alpha) {
        for (int i = 0; i < 64; ++i) tile_writer.write_u16(quant_a.step(i));
        auto alpha_mode_result = write_block_modes(a_blocks);
        if (!alpha_mode_result) {
            return std::unexpected(alpha_mode_result.error());
        }
        auto alpha_span_result = write_spans(alpha_spans);
        if (!alpha_span_result) {
            return std::unexpected(alpha_span_result.error());
        }
        if (use_plane_max_coeff_spans) {
            tile_writer.write_u8(alpha_max_coeff_span);
        }
        tile_writer.write_bytes(final_alpha_payload);
    }

    float score = compute_quality_score(tile_y_data.data(), reconstructed_y.data(), tw, th, max_val);

    TileEncodeResult result;
    result.tile_x = static_cast<uint16_t>(tile_x);
    result.tile_y = static_cast<uint16_t>(tile_y);
    result.compressed = tile_writer.finish();
    result.quality_score = score;
    if (stats != nullptr) {
        // Coefficient-payload split (tables vs coded data): re-encode the
        // final payloads with the winning config. Deterministic, so the
        // re-encoded bytes match the payloads written above; the stats
        // out-param of the encode calls reports the split. Only done
        // when stats are requested, so normal encodes pay nothing.
        LossyCoeffStreamConfig final_coeff_config = selected_coeff_config;
        final_coeff_config.use_table_bank = use_coefficient_table_bank;
        final_coeff_config.use_table_cluster_selection = use_coefficient_table_bank;
        final_coeff_config.elide_single_symbol_streams = use_elide_single_symbol_streams;
        final_coeff_config.use_significance_maps = use_coefficient_significance_maps;
        final_coeff_config.use_adaptive_sign_streams = use_adaptive_coefficient_signs;

        LossyCoeffPayloadStats y_stats{};
        auto y_re = encode_lossy_plane_payload(y_quantized_blocks, y_spans, y_max_coeff_span,
                                               final_coeff_config, &y_stats);
        LossyCoeffPayloadStats chroma_stats{};
        LossyCoeffPayloadStats alpha_stats{};
        if (y_re && y_re->size() == final_y_payload.size()) {
            if (final_use_shared_chroma_coeff_tables) {
                auto c_re = encode_lossy_chroma_payload(cb_quantized_blocks, cr_quantized_blocks,
                                                        chroma_spans, chroma_max_coeff_span,
                                                        final_coeff_config, &chroma_stats);
                if (!c_re || c_re->size() != final_shared_chroma_coeff_payload.size()) {
                    chroma_stats = {};
                }
            } else {
                LossyCoeffPayloadStats cb_stats{};
                LossyCoeffPayloadStats cr_stats{};
                auto cb_re = encode_lossy_plane_payload(cb_quantized_blocks, chroma_spans,
                                                        chroma_max_coeff_span, final_coeff_config, &cb_stats);
                auto cr_re = encode_lossy_plane_payload(cr_quantized_blocks, chroma_spans,
                                                        chroma_max_coeff_span, final_coeff_config, &cr_stats);
                if (cb_re && cr_re &&
                    cb_re->size() == final_independent_cb_coeff_payload.size() &&
                    cr_re->size() == final_independent_cr_coeff_payload.size()) {
                    chroma_stats.flag_bytes = cb_stats.flag_bytes + cr_stats.flag_bytes;
                    chroma_stats.table_bytes = cb_stats.table_bytes + cr_stats.table_bytes;
                    chroma_stats.stream_bytes = cb_stats.stream_bytes + cr_stats.stream_bytes;
                    chroma_stats.total_bytes = cb_stats.total_bytes + cr_stats.total_bytes;
                    chroma_stats.sign_bytes = cb_stats.sign_bytes + cr_stats.sign_bytes;
                    for (size_t i = 0; i < 64; ++i) {
                        chroma_stats.position_stream_bytes[i] =
                            cb_stats.position_stream_bytes[i] + cr_stats.position_stream_bytes[i];
                    }
                } else {
                    chroma_stats = {};
                }
            }
            if (has_alpha) {
                auto a_re = encode_lossy_plane_payload(a_quantized_blocks, alpha_spans,
                                                       alpha_max_coeff_span, final_coeff_config, &alpha_stats);
                if (!a_re || a_re->size() != final_alpha_payload.size()) {
                    alpha_stats = {};
                }
            }
        } else {
            y_stats = {};
        }

        stats->quant_table_bytes = quant_table_bytes + (has_alpha ? 128u : 0u);
        stats->layout_bytes = layout_bytes;
        stats->v7_scale_bytes = v7_scale_bytes;
        stats->mode_bytes = mode_bytes;
        stats->span_bytes = span_bytes;
        stats->max_span_bytes = max_span_bytes;
        stats->y_coeff_table_bytes = y_stats.table_bytes;
        stats->y_coeff_stream_bytes = y_stats.stream_bytes;
        stats->y_coeff_flag_bytes = y_stats.flag_bytes;
        stats->chroma_coeff_table_bytes = chroma_stats.table_bytes;
        stats->chroma_coeff_stream_bytes = chroma_stats.stream_bytes;
        stats->chroma_coeff_flag_bytes = chroma_stats.flag_bytes;
        stats->alpha_coeff_table_bytes = alpha_stats.table_bytes;
        stats->alpha_coeff_stream_bytes = alpha_stats.stream_bytes;
        stats->alpha_coeff_flag_bytes = alpha_stats.flag_bytes;
        stats->y_position_stream_bytes = y_stats.position_stream_bytes;
        stats->chroma_position_stream_bytes = chroma_stats.position_stream_bytes;
        stats->y_sign_bytes = y_stats.sign_bytes;
        stats->chroma_sign_bytes = chroma_stats.sign_bytes;
        stats->total_bytes = result.compressed.size();
        result.stats = *stats;
    }
    return result;
}

Result<std::vector<uint8_t>> encode_impl(const Image& image, const EncoderConfig& config,
                                         EncodeStats* stats = nullptr) {
    uint32_t width = image.width();
    uint32_t height = image.height();

    if (width == 0 || height == 0) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "zero dimensions"});
    }

    // The decoder rejects CICP values the converter does not implement
    // (see parse_fhdr), so the encoder must not produce them either.
    if (config.cicp.matrix != 1 && config.cicp.matrix != 5 && config.cicp.matrix != 6 && config.cicp.matrix != 9) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "unknown CICP matrix coefficient"});
    }
    if (config.cicp.transfer != 1 && config.cicp.transfer != 13 && config.cicp.transfer != 16 && config.cicp.transfer != 18) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "unknown CICP transfer characteristic"});
    }

    auto tile_size_log2 = resolve_tile_size_log2(config);
    if (!tile_size_log2) {
        return std::unexpected(tile_size_log2.error());
    }

    const bool source_has_alpha = image.has_alpha();

    FrameHeader fhdr;
    fhdr.width = width;
    fhdr.height = height;
    fhdr.bit_depth = static_cast<uint8_t>(config.bit_depth);
    fhdr.cicp_primaries = config.cicp.primaries;
    fhdr.cicp_transfer = config.cicp.transfer;
    fhdr.cicp_matrix = config.cicp.matrix;
    fhdr.tile_size_log2 = *tile_size_log2;

    fhdr.flags = 0;
    if (config.lossless) fhdr.flags |= wk::FHDR_FLAG_LOSSLESS;
    if (source_has_alpha) fhdr.flags |= wk::FHDR_FLAG_ALPHA;
    if (config.bit_depth != BitDepth::Bits8) fhdr.flags |= wk::FHDR_FLAG_HDR;
    if (config.cicp.full_range) fhdr.flags |= wk::FHDR_FLAG_FULL_RANGE;
    fhdr.flags |= FHDR_FLAG_TILED;

    WkFile file;
    file.header = fhdr;

    if (config.lossless) {
        std::vector<uint8_t> expanded_rgb;
        const uint8_t* lossless_pixels = image.pixels().data();
        if (!source_has_alpha && config.bit_depth == BitDepth::Bits8) {
            expanded_rgb = expand_rgb8_to_rgba8(image);
            lossless_pixels = expanded_rgb.data();
        }

        auto encoded = lossless_encode(lossless_pixels, width, height, fhdr.bit_depth);
        if (!encoded) return std::unexpected(encoded.error());

        Chunk tile_chunk;
        std::memcpy(tile_chunk.type, CHUNK_TILE, 4);

        TileHeader th;
        th.tile_x = 0;
        th.tile_y = 0;
        th.layer_flags = TILE_HAS_BASE;
        th.compressed_size = static_cast<uint32_t>(encoded->size());

        ByteWriter tw;
        auto th_data = serialize_tile_header(th);
        tw.write_bytes(th_data);
        tw.write_bytes(*encoded);
        tile_chunk.payload = tw.finish();

        file.tile_chunks.push_back(std::move(tile_chunk));
    } else {
        size_t pixel_count = static_cast<size_t>(width) * height;

        std::vector<int16_t> y_plane(pixel_count);
        std::vector<int16_t> cb_plane_full(pixel_count);
        std::vector<int16_t> cr_plane_full(pixel_count);

        rgb_to_ycbcr(y_plane.data(), cb_plane_full.data(), cr_plane_full.data(),
                      image.pixels().data(), width, height,
                      fhdr.cicp_matrix, fhdr.bit_depth, fhdr.full_range(),
                      source_has_alpha);

        std::vector<int16_t> cb_plane;
        std::vector<int16_t> cr_plane;
        std::vector<int16_t> alpha_plane;
        if (source_has_alpha) {
            alpha_plane.resize(pixel_count);
            if (fhdr.bit_depth == 8) {
                const auto pixels = image.pixels();
                for (size_t i = 0; i < pixel_count; ++i) {
                    alpha_plane[i] = pixels[i * 4 + 3];
                }
            } else {
                const uint16_t* pixels = reinterpret_cast<const uint16_t*>(image.pixels().data());
                for (size_t i = 0; i < pixel_count; ++i) {
                    alpha_plane[i] = static_cast<int16_t>(pixels[i * 4 + 3]);
                }
            }
        }

        // YUV420 is undecodable when the first tile is 8x8 or smaller:
        // the decoder infers subsampling from the first tile's chroma
        // block counts, and at <=8px the 4:4:4 and 4:2:0 counts coincide,
        // so it defaults to 4:4:4 and decodes the chroma wrong. FHDR
        // carries no subsampling field, so for tiny images fall back to
        // 4:4:4 instead of writing a file no decoder can read correctly.
        // Normal-size images are unaffected.
        const bool use_yuv420 =
            config.subsampling == Subsampling::YUV420 && (width > 8 || height > 8);

        if (use_yuv420) {
            const uint32_t chroma_w = (width + 1) / 2;
            const uint32_t chroma_h = (height + 1) / 2;
            cb_plane.resize(chroma_w * chroma_h);
            cr_plane.resize(chroma_w * chroma_h);
            subsample_420(cb_plane_full.data(), cb_plane.data(), width, height);
            subsample_420(cr_plane_full.data(), cr_plane.data(), width, height);
        } else {
            cb_plane = std::move(cb_plane_full);
            cr_plane = std::move(cr_plane_full);
        }

        uint32_t tile_size = fhdr.tile_size();
        uint32_t num_tx = fhdr.tiles_x();
        uint32_t num_ty = fhdr.tiles_y();

        ThreadPool pool(config.threads);
        std::vector<std::future<Result<TileEncodeResult>>> futures;

        for (uint32_t ty = 0; ty < num_ty; ty++) {
            for (uint32_t tx = 0; tx < num_tx; tx++) {
                auto future = pool.submit([&, tx, ty]() {
                    // Stats are collected only when the caller asked for
                    // them; otherwise encode_lossy_tile skips the work.
                    TileStats tile_stats{};
                    TileStats* tile_stats_ptr = (stats != nullptr) ? &tile_stats : nullptr;
                    return encode_lossy_tile(
                        y_plane.data(), cb_plane.data(), cr_plane.data(),
                        alpha_plane.empty() ? nullptr : alpha_plane.data(),
                        width, height, tx, ty, tile_size,
                        config.quality, fhdr.bit_depth,
                        source_has_alpha,
                        use_yuv420 ? ChromaSubsampling::YUV420
                                   : ChromaSubsampling::YUV444,
                        config.use_control_field,
                        tile_stats_ptr);
                });
                futures.push_back(std::move(future));
            }
        }

        float total_score = 0;
        int tile_count = 0;

        for (auto& future : futures) {
            auto result = future.get();
            if (!result) return std::unexpected(result.error());

            if (stats != nullptr) {
                stats->tiles.push_back(result->stats);
            }

            Chunk tile_chunk;
            std::memcpy(tile_chunk.type, CHUNK_TILE, 4);

            TileHeader th;
            th.tile_x = result->tile_x;
            th.tile_y = result->tile_y;
            th.layer_flags = static_cast<uint8_t>(TILE_HAS_BASE | (source_has_alpha ? TILE_HAS_ALPHA : 0));
            th.compressed_size = static_cast<uint32_t>(result->compressed.size());

            ByteWriter tw;
            auto th_data = serialize_tile_header(th);
            tw.write_bytes(th_data);
            tw.write_bytes(result->compressed);
            tile_chunk.payload = tw.finish();

            file.tile_chunks.push_back(std::move(tile_chunk));
            total_score += result->quality_score;
            tile_count++;
        }

        if (tile_count > 0) {
            float avg_score = total_score / tile_count;
            meta::MetaBlock meta;
            meta.set(meta::Namespace::Rating, meta::rating::QUALITY_SCORE, avg_score);
            file.metadata = std::move(meta);
            file.header.flags |= wk::FHDR_FLAG_HAS_WKMETA;
        }
    }

    return write_container(file);
}

// Public API boundary: see the decode/get_info wrappers in decoder.cpp.
// Allocation failure must surface as a Result error, never escape.
Result<std::vector<uint8_t>> encode(const Image& image, const EncoderConfig& config) {
    try {
        return encode_impl(image, config);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while encoding"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, std::string("exception while encoding: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, "unknown exception while encoding"});
    }
}

Result<std::vector<uint8_t>> encode_with_stats(const Image& image, const EncoderConfig& config,
                                               EncodeStats& stats) {
    try {
        stats.tiles.clear();
        return encode_impl(image, config, &stats);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while encoding"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, std::string("exception while encoding: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, "unknown exception while encoding"});
    }
}



Image::Image(uint32_t w, uint32_t h, BitDepth bd, bool alpha) {
    info_.width = w;
    info_.height = h;
    info_.bit_depth = bd;
    info_.has_alpha = alpha;

    size_t bpp = bytes_per_pixel();
    pixels_.resize(static_cast<size_t>(w) * h * bpp);
}

uint32_t Image::bytes_per_pixel() const {
    bool is_16 = (info_.bit_depth != BitDepth::Bits8);
    return (info_.has_alpha ? 4u : 3u) * (is_16 ? 2u : 1u);
}

size_t Image::stride() const {
    return static_cast<size_t>(info_.width) * bytes_per_pixel();
}

uint8_t* Image::row(uint32_t y) {
    return pixels_.data() + static_cast<size_t>(y) * stride();
}

const uint8_t* Image::row(uint32_t y) const {
    return pixels_.data() + static_cast<size_t>(y) * stride();
}

}



extern "C" {

void wk_encoder_config_init(wk_encoder_config_t* config) {
    config->quality = 75.0f;
    config->lossless = 0;
    config->bit_depth = 8;
    config->cicp_primaries = 1;
    config->cicp_transfer = 1;
    config->cicp_matrix = 1;
    config->tile_size_log2 = 0;
    config->threads = 0;
    config->target_ssimulacra2 = 0.0f;
    config->chroma_subsampling = 0;
    config->full_range = 1;
}

wk_error_t wk_encode(
    const uint8_t* pixels, uint32_t width, uint32_t height,
    uint32_t pixel_stride, uint8_t bpp,
    const wk_encoder_config_t* config,
    uint8_t** out_data, size_t* out_size) {

    if (!pixels || !config || !out_data || !out_size) {
        return WK_ERROR_INVALID_PARAM;
    }
    if (width == 0 || height == 0) {
        return WK_ERROR_INVALID_PARAM;
    }

    // bit_depth is the caller's claim about the pixel data: anything other
    // than 8/10/12 used to be silently treated as 8-bit, encoding the
    // wrong data without telling the caller.
    if (config->bit_depth != 8 && config->bit_depth != 10 && config->bit_depth != 12) {
        return WK_ERROR_INVALID_PARAM;
    }

    // bpp must agree with bit depth: 3 (RGB) / 4 (RGBA) at 8-bit,
    // 6 / 8 at 10/12-bit. A mismatch used to make the row copies below
    // read/write past the Image rows -- e.g. bpp 6 at 8-bit copied twice
    // the row size (heap overflow).
    const bool is_16bit = config->bit_depth != 8;
    if (bpp != (is_16bit ? 6 : 3) && bpp != (is_16bit ? 8 : 4)) {
        return WK_ERROR_INVALID_PARAM;
    }

    bool has_alpha = (bpp == 4 || bpp == 8);
    const size_t row_bytes = static_cast<size_t>(width) * bpp;
    const size_t src_stride = pixel_stride > 0 ? pixel_stride : row_bytes;
    // An explicit stride smaller than the row would make the row copies
    // overlap. The API has no buffer-size parameter, so no bound on the
    // stride itself can be checked here -- a stride is just a row pitch.
    // The overflow guard below only matters on 32-bit targets: on
    // 64-bit, y < 2^32 and stride < 2^32, so (height-1)*stride can never
    // wrap size_t; on 32-bit it can, and this rejects it.
    if (pixel_stride > 0 && pixel_stride < row_bytes) {
        return WK_ERROR_INVALID_PARAM;
    }
    if (height > 1 && src_stride > (SIZE_MAX - row_bytes) / (height - 1)) {
        return WK_ERROR_INVALID_PARAM;
    }

    wk::BitDepth bd = wk::BitDepth::Bits8;
    if (config->bit_depth == 10) bd = wk::BitDepth::Bits10;
    else if (config->bit_depth == 12) bd = wk::BitDepth::Bits12;

    // The Image constructor allocates width*height*bpp bytes from
    // caller-supplied dimensions; guard it so a huge claim returns
    // WK_ERROR_OOM instead of letting bad_alloc escape extern "C"
    // (which would terminate the caller).
    std::optional<wk::Image> image;
    try {
        image.emplace(width, height, bd, has_alpha);
        for (uint32_t y = 0; y < height; y++) {
            std::memcpy(image->row(y), pixels + y * src_stride, row_bytes);
        }
    } catch (const std::bad_alloc&) {
        return WK_ERROR_OOM;
    } catch (...) {
        return WK_ERROR_ENCODE_FAIL;
    }

    wk::EncoderConfig enc_config;
    enc_config.quality = config->quality;
    enc_config.lossless = config->lossless != 0;
    enc_config.bit_depth = bd;
    enc_config.subsampling = config->chroma_subsampling ?
        wk::Subsampling::YUV420 : wk::Subsampling::YUV444;
    enc_config.cicp = {config->cicp_primaries, config->cicp_transfer,
                       config->cicp_matrix, config->full_range != 0};
    enc_config.tile_size_log2 = config->tile_size_log2;
    enc_config.threads = config->threads;
    enc_config.target_ssimulacra2 = config->target_ssimulacra2;

    auto result = wk::encode(*image, enc_config);
    if (!result) {
        return WK_ERROR_ENCODE_FAIL;
    }

    *out_size = result->size();
    *out_data = static_cast<uint8_t*>(std::malloc(result->size()));
    if (!*out_data) return WK_ERROR_OOM;
    std::memcpy(*out_data, result->data(), result->size());

    return WK_OK;
}

wk_error_t wk_get_info(const uint8_t* data, size_t size, wk_image_info_t* info) {
    if (!data || !info) return WK_ERROR_INVALID_PARAM;

    auto result = wk::get_info({data, size});
    if (!result) return WK_ERROR_INVALID_DATA;

    info->width = result->width;
    info->height = result->height;
    info->bit_depth = static_cast<uint8_t>(result->bit_depth);
    info->cicp_primaries = result->cicp.primaries;
    info->cicp_transfer = result->cicp.transfer;
    info->cicp_matrix = result->cicp.matrix;
    info->flags = 0;
    if (result->is_lossless) info->flags |= wk::FHDR_FLAG_LOSSLESS;
    if (result->is_animated) info->flags |= wk::FHDR_FLAG_ANIMATED;
    if (result->has_alpha) info->flags |= wk::FHDR_FLAG_ALPHA;
    if (result->is_hdr) info->flags |= wk::FHDR_FLAG_HDR;
    if (result->has_wkmeta) info->flags |= wk::FHDR_FLAG_HAS_WKMETA;
    if (result->cicp.full_range) info->flags |= wk::FHDR_FLAG_FULL_RANGE;
    info->tile_size_log2 = static_cast<uint8_t>(std::bit_width(result->tile_size) - 1);
    info->max_cll = result->max_cll;
    info->max_fall = result->max_fall;
    info->has_alpha = result->has_alpha;
    info->is_lossless = result->is_lossless;
    info->is_animated = result->is_animated;
    info->is_hdr = result->is_hdr;
    info->frame_count = result->frame_count;

    return WK_OK;
}

void wk_free(void* ptr) {
    std::free(ptr);
}

const char* wk_version(void) {
    return "0.2.1";
}

}




