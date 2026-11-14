// Phase 2 perceptual control field implementation. See control_field.h.
#include "control_field.h"

#include <algorithm>
#include <cmath>

namespace wk {
namespace {

// Quant scale indices into QuantTable::kQuantScaleFactors.
// 4 = identity (1.0), 5 = 1.25 (coarser).
// NOTE: there is deliberately NO finer scale here. The first mapping
// (Flat -> 0.875 finer, Textured -> 1.25 coarser) was measured and
// REJECTED 2026-11-14: it grew files +24% (q85) to +39% (q75) while
// PSNR fell (-0.33 dB at q75) and SSIM rose only +0.0007 — finer
// quant on smooth blocks is far more expensive in bits than the
// coarser quant on textured blocks saves, the same asymmetry that
// killed the V7 0.5-scale experiment. The seed boost already covers
// the worst banding case (smooth-ramp seed blocks); general flat
// blocks stay at identity, and the field only spends coarser quant
// where masking hides it.
constexpr uint8_t kScaleIdentity = 4;
constexpr uint8_t kScaleCoarser = 5;  // 1.25 — textured blocks, masking

// Classification thresholds on 8-bit luma (0..255). Tuned by
// measurement on the benchmark photos; see the Slice 1 report.
constexpr float kFlatVarianceMax = 25.0f;    // std < 5
constexpr float kFlatGradientMax = 4.0f;
constexpr float kTexturedVarianceMin = 400.0f;  // std > 20
constexpr float kEdgeGradientMin = 48.0f;
constexpr float kEdgeMeanGradientMax = 12.0f;  // concentrated step, not texture

}  // namespace

std::vector<BlockControl> analyze_control_field(
    const int16_t* tile_luma, uint32_t tw, uint32_t th) {
    const uint32_t blocks_per_row = (tw + 7) / 8;
    const uint32_t blocks_per_col = (th + 7) / 8;
    std::vector<BlockControl> field;
    field.reserve(static_cast<size_t>(blocks_per_row) * blocks_per_col);

    for (uint32_t by = 0; by < blocks_per_col; ++by) {
        for (uint32_t bx = 0; bx < blocks_per_row; ++bx) {
            const uint32_t x0 = bx * 8;
            const uint32_t y0 = by * 8;
            const uint32_t bw = std::min(8u, tw - x0);
            const uint32_t bh = std::min(8u, th - y0);
            const size_t n = static_cast<size_t>(bw) * bh;

            // Mean and variance from the block's real pixels.
            double sum = 0.0;
            double sum_sq = 0.0;
            for (uint32_t r = 0; r < bh; ++r) {
                for (uint32_t c = 0; c < bw; ++c) {
                    const double v = tile_luma[(y0 + r) * tw + (x0 + c)];
                    sum += v;
                    sum_sq += v * v;
                }
            }
            const double mean = sum / static_cast<double>(n);
            double variance = sum_sq / static_cast<double>(n) - mean * mean;
            if (variance < 0.0) variance = 0.0;

            // Mean absolute gradient (horizontal + vertical) and the
            // strongest single-pixel step (edge indicator).
            double grad_sum = 0.0;
            size_t grad_count = 0;
            double max_grad = 0.0;
            for (uint32_t r = 0; r < bh; ++r) {
                for (uint32_t c = 0; c < bw; ++c) {
                    const double v = tile_luma[(y0 + r) * tw + (x0 + c)];
                    if (c + 1 < bw) {
                        const double d = std::abs(
                            tile_luma[(y0 + r) * tw + (x0 + c + 1)] - v);
                        grad_sum += d;
                        ++grad_count;
                        max_grad = std::max(max_grad, d);
                    }
                    if (r + 1 < bh) {
                        const double d = std::abs(
                            tile_luma[(y0 + r + 1) * tw + (x0 + c)] - v);
                        grad_sum += d;
                        ++grad_count;
                        max_grad = std::max(max_grad, d);
                    }
                }
            }
            const double mean_grad =
                grad_count == 0 ? 0.0 : grad_sum / static_cast<double>(grad_count);

            BlockControl bc;
            bc.activity = static_cast<float>(
                std::clamp(variance / 400.0, 0.0, 1.0));
            bc.edge_strength = static_cast<float>(
                std::clamp(max_grad / 64.0, 0.0, 1.0));

            if (variance < kFlatVarianceMax && mean_grad < kFlatGradientMax) {
                bc.content = BlockContent::Flat;
                bc.quant_scale_index = kScaleIdentity;
            } else if (max_grad > kEdgeGradientMin && mean_grad < kEdgeMeanGradientMax) {
                // A sharp step in an otherwise flat block: the gradient
                // is concentrated (high max, low mean), unlike texture
                // where gradients are distributed (high mean). Checked
                // before Textured because an edge on a flat background
                // also has high variance (bimodal), which would
                // otherwise misread as texture.
                bc.content = BlockContent::Edge;
                bc.quant_scale_index = kScaleIdentity;
            } else if (variance > kTexturedVarianceMin) {
                bc.content = BlockContent::Textured;
                bc.quant_scale_index = kScaleCoarser;
            } else {
                bc.content = BlockContent::Normal;
                bc.quant_scale_index = kScaleIdentity;
            }
            field.push_back(bc);
        }
    }
    return field;
}

}  // namespace wk
