// Phase 2 perceptual control field implementation. See control_field.h.
#include "control_field.h"

#include <algorithm>
#include <cmath>
#include <queue>

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

// Content-type thresholds on full-range BT.709 YCbCr (0..255).
// Calibrated from colour-cluster probes on the benchmark photos; the
// skin box follows Chai & Ngan (1999) Cb 77..127 / Cr 133..173
// (studio-swing BT.601), re-centred to full-range BT.709.
constexpr double kSkinCbLo = 95, kSkinCbHi = 130;
constexpr double kSkinCrLo = 131, kSkinCrHi = 175;
constexpr double kSkinHueLo = -75, kSkinHueHi = -15;  // atan2(Cb-128, Cr-128)
constexpr double kSkinYLo = 50, kSkinYHi = 220;
constexpr double kSkinChromaStdMin = 2.0;  // uniformity gate: the only
// thing separating skin from a perfectly uniform beige wall (std ~1).
constexpr double kSkyCbMin = 138, kSkyCrMax = 128;
constexpr double kSkyYMin = 140, kSkyVarMax = 100;
constexpr double kFoliageCbMax = 110;
constexpr double kFoliageHueLo = -140, kFoliageHueHi = -65;
constexpr double kFoliageSatMin = 20, kFoliageVarMin = 25;
constexpr double kFlatGraphicsVarMax = 25, kFlatGraphicsSatMin = 35,
                 kFlatGraphicsChromaStdMax = 10;
constexpr double kTextChromaStdMax = 8, kTextVarMin = 100;

// Skin false-positive rule (#skin-fp), measured 2026-11-16: a Skin
// block is dropped from the skin treatment when its component is
// wide and flat, or its chroma gradient is high (see
// skin_false_positive_mask below).
constexpr uint32_t kSkinFpMinCompWidth = 40;
constexpr double kSkinFpMinAspect = 2.0;
constexpr float kSkinFpMinChromaGrad = 3.0f;

}  // namespace

std::vector<BlockControl> analyze_control_field(
    const int16_t* tile_luma, uint32_t tw, uint32_t th,
    const int16_t* tile_cb, const int16_t* tile_cr, uint8_t bit_depth) {
    // Bit-depth recalibration (#hdr): every threshold below is
    // calibrated in 8-bit code values. Above 8-bit they scale with
    // the code-value range — gradients and colours x v, variances
    // x v^2 (v = 2^(bd-8): 4 at 10-bit, 16 at 12-bit). Measured
    // slice 0, 2026-11-17: 100.00% classification agreement on
    // 176,697 blocks, Skin/TextUI recall 100%; 98.44% under
    // BT.2020. At 8-bit v = 1: behaviour identical.
    const double v =
        static_cast<double>(1 << std::max(0, static_cast<int>(bit_depth) - 8));
    const double v2 = v * v;
    const double mid = 128.0 * v;
    const bool use_color = tile_cb != nullptr && tile_cr != nullptr;
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
                std::clamp(variance / (400.0 * v2), 0.0, 1.0));
            bc.edge_strength = static_cast<float>(
                std::clamp(max_grad / (64.0 * v), 0.0, 1.0));

            if (variance < kFlatVarianceMax * v2 && mean_grad < kFlatGradientMax * v) {
                bc.content = BlockContent::Flat;
                bc.quant_scale_index = kScaleIdentity;
            } else if (max_grad > kEdgeGradientMin * v && mean_grad < kEdgeMeanGradientMax * v) {
                // A sharp step in an otherwise flat block: the gradient
                // is concentrated (high max, low mean), unlike texture
                // where gradients are distributed (high mean). Checked
                // before Textured because an edge on a flat background
                // also has high variance (bimodal), which would
                // otherwise misread as texture.
                bc.content = BlockContent::Edge;
                bc.quant_scale_index = kScaleIdentity;
            } else if (variance > kTexturedVarianceMin * v2) {
                bc.content = BlockContent::Textured;
                bc.quant_scale_index = kScaleCoarser;
            } else {
                bc.content = BlockContent::Normal;
                bc.quant_scale_index = kScaleIdentity;
            }

            if (use_color) {
                // Colour features over the 8x8 block, coordinates
                // clamped to the tile bounds (same edge policy as the
                // research diagnostic: a tile that is the whole image
                // reproduces its classification exactly). This is a
                // second pass over the block's pixels, deliberately
                // separate from the luma pass above so the v1
                // arithmetic — and its results — stay untouched.
                double sum_y = 0.0, sum_yy = 0.0;
                double sum_cb = 0.0, sum_cb2 = 0.0;
                double sum_cr = 0.0, sum_cr2 = 0.0;
                double sum_sat = 0.0;
                double grad_sum = 0.0;
                size_t grad_count = 0;
                double max_grad = 0.0;
                double cgrad_sum = 0.0;
                auto at = [&](const int16_t* plane, int x, int y) -> double {
                    x = std::clamp(x, 0, static_cast<int>(tw) - 1);
                    y = std::clamp(y, 0, static_cast<int>(th) - 1);
                    return plane[static_cast<size_t>(y) * tw + x];
                };
                for (uint32_t r = 0; r < 8; ++r) {
                    for (uint32_t c = 0; c < 8; ++c) {
                        const int x = static_cast<int>(x0 + c);
                        const int yy = static_cast<int>(y0 + r);
                        const double yv = at(tile_luma, x, yy);
                        const double cbv = at(tile_cb, x, yy);
                        const double crv = at(tile_cr, x, yy);
                        sum_y += yv;
                        sum_yy += yv * yv;
                        sum_cb += cbv;
                        sum_cb2 += cbv * cbv;
                        sum_cr += crv;
                        sum_cr2 += crv * crv;
                        sum_sat += std::sqrt((cbv - mid) * (cbv - mid) +
                                             (crv - mid) * (crv - mid));
                        if (c < 7) {
                            const double d = std::abs(at(tile_luma, x + 1, yy) - yv);
                            grad_sum += d;
                            ++grad_count;
                            max_grad = std::max(max_grad, d);
                            cgrad_sum += std::abs(at(tile_cb, x + 1, yy) - cbv) +
                                         std::abs(at(tile_cr, x + 1, yy) - crv);
                        }
                        if (r < 7) {
                            const double d = std::abs(at(tile_luma, x, yy + 1) - yv);
                            grad_sum += d;
                            ++grad_count;
                            max_grad = std::max(max_grad, d);
                            cgrad_sum += std::abs(at(tile_cb, x, yy + 1) - cbv) +
                                         std::abs(at(tile_cr, x, yy + 1) - crv);
                        }
                    }
                }
                const double n = 64.0;
                const double y_mean = sum_y / n;
                const double y_var =
                    std::max(0.0, sum_yy / n - y_mean * y_mean);
                const double mean_grad =
                    grad_count == 0 ? 0.0 : grad_sum / static_cast<double>(grad_count);
                // Mean chroma gradient over the same adjacent pairs
                // (diagnostic definition: mean of |dCb|+|dCr| / 2).
                bc.chroma_grad = static_cast<float>(
                    grad_count == 0
                        ? 0.0
                        : cgrad_sum / (2.0 * static_cast<double>(grad_count)));
                const double cb_mean = sum_cb / n;
                const double cr_mean = sum_cr / n;
                const double cb_std = std::sqrt(
                    std::max(0.0, sum_cb2 / n - cb_mean * cb_mean));
                const double cr_std = std::sqrt(
                    std::max(0.0, sum_cr2 / n - cr_mean * cr_mean));
                const double sat_mean = sum_sat / n;
                const double hue =
                    std::atan2(cb_mean - mid, cr_mean - mid) * 180.0 / M_PI;
                const bool skin_color =
                    cr_mean >= kSkinCrLo * v && cr_mean <= kSkinCrHi * v &&
                    cb_mean >= kSkinCbLo * v && cb_mean <= kSkinCbHi * v &&
                    hue >= kSkinHueLo && hue <= kSkinHueHi &&
                    y_mean >= kSkinYLo * v && y_mean <= kSkinYHi * v;
                const bool skin_uniform_ok =
                    cb_std > kSkinChromaStdMin * v || cr_std > kSkinChromaStdMin * v;

                // Priority (first match wins): Skin, TextUI, Sky,
                // Foliage, FlatGraphics, then the luma class above.
                // Skin is checked first: text is black/white/red/blue
                // and never passes the skin colour box, so it costs
                // text nothing and recovers face blocks whose edges
                // would otherwise read as TextUI.
                if (skin_color && skin_uniform_ok) {
                    bc.content = BlockContent::Skin;
                } else if (max_grad > kEdgeGradientMin * v &&
                           mean_grad < kEdgeMeanGradientMax * v &&
                           cb_std < kTextChromaStdMax * v &&
                           cr_std < kTextChromaStdMax * v &&
                           y_var >= kTextVarMin * v2) {
                    bc.content = BlockContent::TextUI;
                } else if (cb_mean >= kSkyCbMin * v && cr_mean <= kSkyCrMax * v &&
                           y_mean >= kSkyYMin * v && y_var < kSkyVarMax * v2) {
                    bc.content = BlockContent::Sky;
                } else if (cb_mean <= kFoliageCbMax * v &&
                           hue >= kFoliageHueLo && hue <= kFoliageHueHi &&
                           sat_mean >= kFoliageSatMin * v &&
                           y_var >= kFoliageVarMin * v2) {
                    bc.content = BlockContent::Foliage;
                } else if (y_var < kFlatGraphicsVarMax * v2 &&
                           sat_mean >= kFlatGraphicsSatMin * v &&
                           cb_std < kFlatGraphicsChromaStdMax * v &&
                           cr_std < kFlatGraphicsChromaStdMax * v) {
                    bc.content = BlockContent::FlatGraphics;
                }
                // Otherwise the luma class stands. quant_scale_index
                // is deliberately left as the luma class set it: the
                // content-type classes are informational only.
            }
            field.push_back(bc);
        }
    }
    return field;
}

std::vector<uint8_t> skin_false_positive_mask(
    const std::vector<BlockControl>& field, uint32_t blocks_per_row,
    uint32_t blocks_per_col, uint8_t bit_depth) {
    std::vector<uint8_t> mask(field.size(), 0);
    if (field.empty() || blocks_per_row == 0 || blocks_per_col == 0 ||
        field.size() != static_cast<size_t>(blocks_per_row) * blocks_per_col)
        return mask;

    // The chroma-grad threshold is 8-bit-calibrated (3.0); it scales
    // with the code-value range like the field's (x4 at 10-bit).
    const float cgrad_threshold = static_cast<float>(
        kSkinFpMinChromaGrad *
        (1 << std::max(0, static_cast<int>(bit_depth) - 8)));

    // 8-connected components over the Skin mask (same connectivity as
    // the research diagnostic). A component is described by its
    // bounding box only: the rule cares about width and aspect.
    struct CompBox {
        uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    std::vector<int32_t> comp(field.size(), -1);
    std::vector<CompBox> boxes;
    const int dx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (uint32_t y0 = 0; y0 < blocks_per_col; ++y0) {
        for (uint32_t x0 = 0; x0 < blocks_per_row; ++x0) {
            const size_t i0 = static_cast<size_t>(y0) * blocks_per_row + x0;
            if (field[i0].content != BlockContent::Skin || comp[i0] >= 0)
                continue;
            CompBox box{x0, y0, x0, y0};
            std::queue<size_t> q;
            q.push(i0);
            comp[i0] = static_cast<int32_t>(boxes.size());
            while (!q.empty()) {
                const size_t i = q.front();
                q.pop();
                const uint32_t cx = static_cast<uint32_t>(i % blocks_per_row);
                const uint32_t cy = static_cast<uint32_t>(i / blocks_per_row);
                box.x0 = std::min(box.x0, cx);
                box.x1 = std::max(box.x1, cx);
                box.y0 = std::min(box.y0, cy);
                box.y1 = std::max(box.y1, cy);
                for (int d = 0; d < 8; ++d) {
                    const int nx = static_cast<int>(cx) + dx8[d];
                    const int ny = static_cast<int>(cy) + dy8[d];
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(blocks_per_row) ||
                        ny >= static_cast<int>(blocks_per_col))
                        continue;
                    const size_t ni =
                        static_cast<size_t>(ny) * blocks_per_row + nx;
                    if (field[ni].content == BlockContent::Skin && comp[ni] < 0) {
                        comp[ni] = static_cast<int32_t>(boxes.size());
                        q.push(ni);
                    }
                }
            }
            boxes.push_back(box);
        }
    }

    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i].content != BlockContent::Skin || comp[i] < 0) continue;
        const CompBox& box = boxes[static_cast<size_t>(comp[i])];
        const uint32_t w = box.x1 - box.x0 + 1;
        const uint32_t h = box.y1 - box.y0 + 1;
        const bool wide_flat =
            w >= kSkinFpMinCompWidth &&
            static_cast<double>(w) / static_cast<double>(h) >= kSkinFpMinAspect;
        const bool rough_chroma =
            field[i].chroma_grad >= cgrad_threshold;
        mask[i] = (wide_flat || rough_chroma) ? 1 : 0;
    }
    return mask;
}

}  // namespace wk
