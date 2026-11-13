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

#include "predict.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace wk
{

    static inline int16_t clamp_px(int v, int16_t max_val)
    {
        return static_cast<int16_t>(std::clamp(v, 0, static_cast<int>(max_val)));
    }

    void predict_8x8(PredMode mode, const int16_t *above, const int16_t *left,
                     int16_t above_left, int16_t *pred, int16_t max_val)
    {

        int16_t default_above[8], default_left[8];
        int16_t mid = static_cast<int16_t>((static_cast<int>(max_val) + 1) / 2);

        if (!above)
        {
            std::fill(default_above, default_above + 8, mid);
            above = default_above;
        }
        if (!left)
        {
            std::fill(default_left, default_left + 8, mid);
            left = default_left;
        }

        switch (mode)
        {
        case PredMode::DC:
        {
            int sum = 0;
            for (int i = 0; i < 8; i++)
                sum += above[i] + left[i];
            int16_t dc = static_cast<int16_t>((sum + 8) / 16);
            std::fill(pred, pred + 64, dc);
            break;
        }
        case PredMode::V:
        {
            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    pred[r * 8 + c] = above[c];
                }
            }
            break;
        }
        case PredMode::H:
        {
            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    pred[r * 8 + c] = left[r];
                }
            }
            break;
        }
        case PredMode::TM:
        {

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int v = static_cast<int>(left[r]) + static_cast<int>(above[c]) - static_cast<int>(above_left);
                    pred[r * 8 + c] = clamp_px(v, max_val);
                }
            }
            break;
        }
        case PredMode::DC_LEFT:
        {
            int sum = 0;
            for (int i = 0; i < 8; i++)
                sum += left[i];
            int16_t dc = static_cast<int16_t>((sum + 4) / 8);
            std::fill(pred, pred + 64, dc);
            break;
        }
        case PredMode::DC_TOP:
        {
            int sum = 0;
            for (int i = 0; i < 8; i++)
                sum += above[i];
            int16_t dc = static_cast<int16_t>((sum + 4) / 8);
            std::fill(pred, pred + 64, dc);
            break;
        }
        case PredMode::DC_128:
        {
            std::fill(pred, pred + 64, mid);
            break;
        }
        case PredMode::D45:
        {

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int idx = c + r + 1;
                    if (idx < 8)
                    {
                        pred[r * 8 + c] = above[idx];
                    }
                    else
                    {
                        pred[r * 8 + c] = above[7];
                    }
                }
            }
            break;
        }
        case PredMode::D135:
        {

            int16_t ref[17];
            for (int i = 0; i < 8; i++)
                ref[i] = left[7 - i];
            ref[8] = above_left;
            for (int i = 0; i < 8; i++)
                ref[9 + i] = above[i];

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int idx = 8 + c - r;
                    idx = std::clamp(idx, 0, 16);
                    pred[r * 8 + c] = ref[idx];
                }
            }
            break;
        }
        case PredMode::D117:
        {

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int idx = c - (r >> 1);
                    if (idx >= 0 && idx < 8)
                    {
                        if (r & 1)
                        {
                            pred[r * 8 + c] = clamp_px(
                                (above[std::max(0, idx - 1)] + above[idx] + 1) / 2,
                                max_val);
                        }
                        else
                        {
                            pred[r * 8 + c] = above[idx];
                        }
                    }
                    else if (idx < 0)
                    {
                        int li = r + ((-idx - 1) << 1);
                        pred[r * 8 + c] = (li >= 0 && li < 8) ? left[li] : left[7];
                    }
                    else
                    {
                        pred[r * 8 + c] = above[7];
                    }
                }
            }
            break;
        }
        case PredMode::D153:
        {

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int idx = r - (c >> 1);
                    if (idx >= 0 && idx < 8)
                    {
                        if (c & 1)
                        {
                            pred[r * 8 + c] = clamp_px(
                                (left[std::max(0, idx - 1)] + left[idx] + 1) / 2,
                                max_val);
                        }
                        else
                        {
                            pred[r * 8 + c] = left[idx];
                        }
                    }
                    else if (idx < 0)
                    {
                        int ai = c + ((-idx - 1) << 1);
                        pred[r * 8 + c] = (ai >= 0 && ai < 8) ? above[ai] : above[7];
                    }
                    else
                    {
                        pred[r * 8 + c] = left[7];
                    }
                }
            }
            break;
        }
        case PredMode::D207:
        {

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int idx = r + (c >> 1);
                    if (idx < 8)
                    {
                        if (c & 1)
                        {
                            int next = std::min(idx + 1, 7);
                            pred[r * 8 + c] = clamp_px(
                                (left[idx] + left[next] + 1) / 2, max_val);
                        }
                        else
                        {
                            pred[r * 8 + c] = left[idx];
                        }
                    }
                    else
                    {
                        pred[r * 8 + c] = left[7];
                    }
                }
            }
            break;
        }
        case PredMode::D63:
        {

            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    int idx = c + (r >> 1);
                    if (idx < 8)
                    {
                        if (r & 1)
                        {
                            int next = std::min(idx + 1, 7);
                            pred[r * 8 + c] = clamp_px(
                                (above[idx] + above[next] + 1) / 2, max_val);
                        }
                        else
                        {
                            pred[r * 8 + c] = above[idx];
                        }
                    }
                    else
                    {
                        pred[r * 8 + c] = above[7];
                    }
                }
            }
            break;
        }
        case PredMode::PLANAR:
        {
            // HEVC-style planar: a weighted average of the horizontal
            // and vertical linear interpolations between the references.
            // A smooth 2D ramp is predicted as a ramp, not a flat copy,
            // so the slope survives even when the residual quantizes to
            // zero. The four weights sum to 16, so (sum + 8) >> 4 is the
            // rounded average; every term is in [0, max_val], so the
            // result is in range without clamping. Requires layout V7.
            const int top_right = above[7];
            const int bottom_left = left[7];
            for (int r = 0; r < 8; r++)
            {
                for (int c = 0; c < 8; c++)
                {
                    const int value = (7 - c) * static_cast<int>(left[r]) +
                                      (c + 1) * top_right +
                                      (7 - r) * static_cast<int>(above[c]) +
                                      (r + 1) * bottom_left + 8;
                    pred[r * 8 + c] = static_cast<int16_t>(value >> 4);
                }
            }
            break;
        }
        default:
            std::fill(pred, pred + 64, mid);
            break;
        }
    }

    PredMode select_deterministic_mode(const int16_t *above, const int16_t *left,
                                       int16_t above_left, int16_t max_val)
    {
        const bool has_above = above != nullptr;
        const bool has_left = left != nullptr;
        if (has_above && !has_left)
        {
            return PredMode::V;
        }
        if (!has_above && has_left)
        {
            return PredMode::H;
        }
        if (!has_above && !has_left)
        {
            return PredMode::DC_128;
        }

        auto mean_edge = [](const int16_t *edge) -> int
        {
            int sum = 0;
            for (int i = 0; i < 8; ++i)
            {
                sum += edge[i];
            }
            return (sum + 4) / 8;
        };

        auto variation = [](const int16_t *edge) -> int
        {
            int sum = 0;
            for (int i = 1; i < 8; ++i)
            {
                sum += std::abs(static_cast<int>(edge[i]) - static_cast<int>(edge[i - 1]));
            }
            return sum;
        };

        const int above_mean = mean_edge(above);
        const int left_mean = mean_edge(left);
        const int edge_delta = std::abs(above_mean - left_mean);
        const int corner_delta = std::abs(static_cast<int>(above[0]) - static_cast<int>(above_left)) +
                                 std::abs(static_cast<int>(left[0]) - static_cast<int>(above_left));
        const int above_variation = variation(above);
        const int left_variation = variation(left);

        const int smooth_threshold = std::max(4, static_cast<int>(std::lround(max_val * 0.025)));
        const int variation_threshold = std::max(8, static_cast<int>(std::lround(max_val * 0.080)));

        if (edge_delta <= smooth_threshold &&
            corner_delta <= smooth_threshold * 2 &&
            above_variation <= variation_threshold &&
            left_variation <= variation_threshold)
        {
            return PredMode::TM;
        }

        if (above_variation + edge_delta * 2 < left_variation)
        {
            return PredMode::V;
        }
        if (left_variation + edge_delta * 2 < above_variation)
        {
            return PredMode::H;
        }
        if (edge_delta <= smooth_threshold * 2 && corner_delta <= variation_threshold)
        {
            return PredMode::TM;
        }

        return PredMode::DC;
    }

    float compute_ssd_8x8(const int16_t *a, const int16_t *b)
    {
        float ssd = 0;
        for (int i = 0; i < 64; i++)
        {
            float d = static_cast<float>(a[i]) - static_cast<float>(b[i]);
            ssd += d * d;
        }
        return ssd;
    }

    float estimate_bits_8x8(const int16_t *residual)
    {

        float bits = 0;
        int nonzero = 0;
        for (int i = 0; i < 64; i++)
        {
            if (residual[i] != 0)
            {
                bits += std::log2(std::abs(static_cast<float>(residual[i])) + 1.0f) + 1.0f;
                nonzero++;
            }
        }

        bits += 4.0f;

        bits += 1.0f;
        return bits;
    }

    // Quantization-aware rate-distortion: for each mode, the residual is
    // transformed, quantized (with the plane's table and the block's
    // scale), clamped to the stream range, dequantized, and added back
    // to the prediction -- exactly as the encoder's in-loop
    // reconstruction does it. The cost is the SSD of the RECONSTRUCTED
    // block (what the decoder will actually produce) plus lambda times
    // the bit estimate of the QUANTIZED coefficients (what the stream
    // will actually carry). The old version measured the SSD of the
    // prediction and the bits of the unquantized residual, so it picked
    // modes whose residuals looked cheap before quantization but were
    // expensive (or lossy) after it -- the cause of planar regressing
    // photos and of the quality non-monotonicity.
    RdResult select_best_mode(const int16_t *original, const int16_t *above,
                              const int16_t *left, int16_t above_left,
                              float lambda, int16_t max_val,
                              const QuantTable &quant, uint8_t bit_depth,
                              float quant_scale)
    {
        RdResult best{PredMode::DC, std::numeric_limits<float>::max()};

        int16_t pred[64];
        int16_t residual[64];
        int16_t reconstructed[64];

        for (int m = 0; m < static_cast<int>(PredMode::NUM_MODES); m++)
        {
            PredMode mode = static_cast<PredMode>(m);

            predict_8x8(mode, above, left, above_left, pred, max_val);

            for (int i = 0; i < 64; i++)
            {
                residual[i] = original[i] - pred[i];
            }

            DctBlock dct_coeffs;
            for (int i = 0; i < 64; ++i)
            {
                dct_coeffs[i] = static_cast<float>(residual[i]);
            }
            dct_2d_forward(dct_coeffs);
            DctBlockI16 quantized;
            quant.quantize_scaled(dct_coeffs, quantized, quant_scale);
            clamp_quantized_to_stream_range(quantized, bit_depth);
            DctBlock recon_dct;
            quant.dequantize_scaled(quantized, recon_dct, quant_scale);
            dct_2d_inverse(recon_dct);

            for (int i = 0; i < 64; ++i)
            {
                const int value = static_cast<int>(std::lround(recon_dct[i])) + pred[i];
                reconstructed[i] = static_cast<int16_t>(std::clamp(value, 0, static_cast<int>(max_val)));
            }

            float ssd = compute_ssd_8x8(original, reconstructed);
            float bits = estimate_bits_8x8(quantized.data());
            float cost = ssd + lambda * bits;

            if (cost < best.cost)
            {
                best.mode = mode;
                best.cost = cost;
            }
        }

        return best;
    }

}
