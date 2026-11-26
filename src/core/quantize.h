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
#pragma once

#include "common.h"
#include "dct.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace wk {

// The JPEG base tables above run to 121 (luma) / 112 (chroma) at the
// highest frequencies. Those large steps resonate with the near-integer
// DCT coefficients of photo blocks: the rounding error is not monotonic
// in the step size, so a finer step can score WORSE. Measured 2026-11-13
// on real photos, the encoder's PSNR fell as quality rose (mira q75
// 55.15 dB -> q80 44.77 dB -> q90 48.10 dB; a single 8x8 block's luma SSE
// went 5.4 -> 834.8 -> 517.7 because q75's steps divide its coefficients
// almost exactly while q80's do not). Capping the base at 40 keeps the
// high-frequency steps small enough that the error stays monotonic in
// quality (verified q50..q100 on mira/ember/horizon); the absolute q75
// PSNR drops because the old value was a resonance spike, not real
// quality. Encoder-side only: tables are stored per tile in the file, so
// old .wk files still decode bit-identically.
constexpr float kMaxBaseQuantValue = 40.0f;

constexpr std::array<uint16_t, 64> BASE_QUANT_LUMA = {{
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
}};

constexpr std::array<uint16_t, 64> BASE_QUANT_CHROMA = {{
    15, 16, 19, 26, 36, 48, 58, 66,
    16, 18, 21, 30, 40, 54, 64, 72,
    19, 21, 26, 36, 48, 60, 70, 78,
    26, 30, 36, 46, 58, 70, 80, 88,
    36, 40, 48, 58, 70, 82, 90, 96,
    48, 54, 60, 70, 82, 92, 98, 102,
    58, 64, 70, 80, 90, 98, 104, 108,
    66, 72, 78, 88, 96, 102, 108, 112
}};

class QuantTable {
public:
    QuantTable() = default;

    // Luma + chroma quant-table shape (#qtab): the q50 size table from
    // the 2026-11-26 encoder-side coordinate descent at q50 (size
    // criterion, ramp constraints), replacing the pass-2 table of
    // 2026-11-25. Band multipliers over zigzag bands [DC, 1-3, 4-7,
    // 8-15, 16-31, 32-63]: Y [1.00; 1.00; 1.25; 1.25; 1.00; 1.00],
    // C [1.25; 1.00; 1.00; 1.25; 1.25; 1.25]. Luma applies as a
    // per-position ratio against the nominal q75 luma table:
    //   new[i] = lround(pass2[i] * built[i] / nominal75[i])
    // where built[i] is the unshaped step at the tile's (possibly
    // adapted) quality. This is the measured table rescaled per tile
    // by the same quality adaptation that built the nominal table.
    // Why ratios, not the band multipliers the descent searched
    // over: applying the multipliers directly rounds differently on
    // quality-adapted tiles and costs bytes on the q75 corpus for
    // the pass-1 table (+1,109 B, measured 2026-11-25). The ratios
    // reproduce the measured q75 table exactly, on every tile.
    // Chroma applies as a multiplier on the nominal table at the
    // tile's bit depth, NOT as a ratio against the q75 table scaled
    // by bd_mul:
    //   new[i] = lround(built[i] * pass2c[i] / nominalC75[i])
    // The two formulas must not be unified: the nominal chroma table
    // does not scale by exactly 4/16 (truncation), so scaling the
    // chroma fit by bd_mul silently smooths chroma by up to 20% at
    // 12-bit (measured 2026-11-25: the pass-1 12-bit gradient SSIM
    // read 0.98991 instead of the true 0.98831). The fit is scaled
    // to the current bit depth through built[i], so the q75 table is
    // the shape table at every depth. The alpha plane uses the
    // unshaped luma table (apply_quant_shape = false at its call
    // site): its steps were not part of the measurement, so they stay
    // exactly as before.
    static constexpr std::array<uint16_t, 64> kQuantPass2Luma = {{
        8, 5, 6, 8, 10, 8, 12, 11,
        10, 11, 14, 12, 12, 14, 19, 31,
        10, 10, 9, 9, 10, 16, 14, 14,
        11, 16, 16, 16, 16, 16, 16, 16,
        14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14}};

    static constexpr std::array<uint16_t, 64> kQuantPass2Chroma = {{
        6, 6, 6, 8, 6, 6, 8, 7,
        8, 10, 14, 11, 10, 11, 14, 15,
        15, 14, 14, 15, 15, 15, 15, 15,
        19, 19, 19, 19, 19, 19, 19, 19,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15}};

    // Nominal chroma table at q75, 8-bit (the divisor for the chroma
    // multiplier above). Verified against build(75, true, 8) with the
    // shape disabled.
    static constexpr std::array<uint16_t, 64> kQuantNominalChromaQ75 = {{
        5, 5, 5, 6, 6, 6, 8, 7,
        6, 8, 11, 9, 8, 9, 11, 12,
        12, 11, 11, 12, 12, 12, 12, 12,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15,
        15, 15, 15, 15, 15, 15, 15, 15}};

    void build(float quality, bool is_chroma, uint8_t bit_depth,
              bool refine_low_frequencies = false,
              bool apply_quant_shape = true) {
        quality_ = std::clamp(quality, 1.0f, 100.0f);
        is_chroma_ = is_chroma;
        bit_depth_ = bit_depth;
        refine_low_frequencies_ = refine_low_frequencies;
        const auto& base = is_chroma ? BASE_QUANT_CHROMA : BASE_QUANT_LUMA;

        const float q = quality_;
        float scale;
        if (q < 50.0f) {
            scale = 5000.0f / q;
        } else {
            scale = 200.0f - 2.0f * q;
        }
        scale = std::max(scale, 1.0f);

        float bd_mul = 1.0f;
        if (bit_depth == 10) bd_mul = 4.0f;
        else if (bit_depth == 12) bd_mul = 16.0f;

        for (int i = 0; i < 64; i++) {
            const int natural_index = ZIGZAG_ORDER[i];
            float base_value = static_cast<float>(base[natural_index]);
            // Cap the base table (see kMaxBaseQuantValue): without it the
            // large high-frequency steps resonate with photo DCT
            // coefficients and PSNR becomes non-monotonic in quality.
            base_value = std::min(base_value, kMaxBaseQuantValue);
            if (is_chroma_) {
                base_value *= chroma_step_scale(i);
            }
            const float v = (base_value * scale / 100.0f) * bd_mul;
            table_[i] = static_cast<uint16_t>(std::max(v, 1.0f));
            if (apply_quant_shape && !is_chroma_) {
                // Nominal q75 step at this bit depth (same formula with
                // scale 50), for the #qtab ratio above.
                const float v75 = (base_value * 50.0f / 100.0f) * bd_mul;
                const uint16_t nominal75 =
                    static_cast<uint16_t>(std::max(v75, 1.0f));
                const double fit =
                    static_cast<double>(kQuantPass2Luma[static_cast<size_t>(i)]) *
                    static_cast<double>(bd_mul);
                table_[i] = static_cast<uint16_t>(std::max(
                    static_cast<int>(std::lround(
                        fit * static_cast<double>(table_[i]) /
                        static_cast<double>(nominal75))),
                    1));
            } else if (apply_quant_shape && is_chroma_) {
                // Chroma multiplier semantics (#qtab): the shaped step is
                // the built step times the pass-2/nominal ratio at q75
                // 8-bit. The built step already carries this tile's
                // quality adaptation and bit depth, so no bd_mul is
                // applied to the fit here — scaling the fit by bd_mul
                // would silently smooth chroma at 10/12-bit (see the
                // comment on kQuantPass2Chroma above).
                table_[i] = static_cast<uint16_t>(std::max(
                    static_cast<int>(std::lround(
                        static_cast<double>(table_[i]) *
                        static_cast<double>(kQuantPass2Chroma[static_cast<size_t>(i)]) /
                        static_cast<double>(kQuantNominalChromaQ75[static_cast<size_t>(i)]))),
                    1));
            }
            inv_table_[i] = 1.0f / table_[i];
        }
    }

    void quantize(const DctBlock& input, DctBlockI16& output) const {
        for (int i = 0; i < 64; i++) {
            const int zi = ZIGZAG_ORDER[i];
            const float v = input[zi];
            const float step = static_cast<float>(table_[i]);
            if (std::abs(v) < step * deadzone_scale(i)) {
                output[i] = 0;
            } else {
                const float q = v * inv_table_[i];
                output[i] = static_cast<int16_t>(
                    (v > 0) ? std::floor(q + 0.5f) : std::ceil(q - 0.5f));
                zero_chroma_hf_deadzone(i, output[i]);  // #k1
            }
        }
    }

    void dequantize(const DctBlockI16& input, DctBlock& output) const {
        for (int i = 0; i < 64; i++) {
            const int zi = ZIGZAG_ORDER[i];
            output[zi] = static_cast<float>(input[i]) * table_[i];
        }
    }

    // Adaptive quantization (layout V7): per-block step multipliers.
    // Every factor is exactly representable in binary floating point,
    // so encoder and decoder agree bit-for-bit. The scaled step applies
    // to the deadzone test, the quantize rounding, and the dequantize
    // multiplication alike; scale 1.0 reproduces quantize/dequantize.
    static constexpr std::array<float, 8> kQuantScaleFactors = {
        0.5f, 0.625f, 0.75f, 0.875f, 1.0f, 1.25f, 1.5f, 2.0f};
    static constexpr uint8_t kQuantScaleIdentity = 4;

    // Chroma high-frequency deadzone (#k1): after rounding, a chroma
    // coefficient at zigzag index >= 24 with |q| <= 1 is zeroed. Luma
    // is untouched. The zeroing happens inside quantization, so the
    // quantization-aware RDO and the reconstruction used for prediction
    // both see the zeroed coefficients. 8-bit only: the rule is
    // measured on 8-bit photos, and at 10/12-bit it costs ~2.2 dB on
    // gradient textures (the |q| = 1 coefficients there are signal,
    // not ringing), so higher bit depths keep the old behavior.
    static constexpr int kChromaHfDeadzoneZigzag = 24;
    static constexpr int16_t kChromaHfDeadzoneMaxQ = 1;

    void quantize_scaled(const DctBlock& input, DctBlockI16& output, float scale) const {
        for (int i = 0; i < 64; i++) {
            const int zi = ZIGZAG_ORDER[i];
            const float v = input[zi];
            const float step = static_cast<float>(table_[i]) * scale;
            if (std::abs(v) < step * deadzone_scale(i)) {
                output[i] = 0;
            } else {
                const float q = v * (inv_table_[i] / scale);
                output[i] = static_cast<int16_t>(
                    (v > 0) ? std::floor(q + 0.5f) : std::ceil(q - 0.5f));
                zero_chroma_hf_deadzone(i, output[i]);  // #k1
            }
        }
    }

    void dequantize_scaled(const DctBlockI16& input, DctBlock& output, float scale) const {
        for (int i = 0; i < 64; i++) {
            const int zi = ZIGZAG_ORDER[i];
            output[zi] = static_cast<float>(input[i]) * table_[i] * scale;
        }
    }

    [[nodiscard]] uint16_t step(int i) const { return table_[i]; }
    [[nodiscard]] const std::array<uint16_t, 64>& steps() const { return table_; }

private:
    void zero_chroma_hf_deadzone(int zigzag_index, int16_t& q) const {
        if (bit_depth_ != 8) return;  // #k1: measured on 8-bit only
        if (is_chroma_ && zigzag_index >= kChromaHfDeadzoneZigzag &&
            q >= -kChromaHfDeadzoneMaxQ && q <= kChromaHfDeadzoneMaxQ) {
            q = 0;
        }
    }

    [[nodiscard]] float chroma_step_scale(int zigzag_index) const {
        if (!is_chroma_) {
            return 1.0f;
        }

        float scale;
        if (quality_ >= 90.0f) {
            if (zigzag_index == 0) scale = 0.58f;
            else if (zigzag_index < 8) scale = 0.56f;
            else if (zigzag_index < 24) scale = 0.58f;
            else scale = 0.72f;
        } else if (quality_ >= 85.0f) {
            if (zigzag_index == 0) scale = 0.64f;
            else if (zigzag_index < 8) scale = 0.62f;
            else if (zigzag_index < 24) scale = 0.63f;
            else scale = 0.78f;
        } else if (quality_ >= 75.0f) {
            if (zigzag_index == 0) scale = 0.72f;
            else if (zigzag_index < 8) scale = 0.68f;
            else if (zigzag_index < 24) scale = 0.64f;
            else scale = 0.76f;
        } else {
            if (zigzag_index == 0) scale = 0.82f;
            else if (zigzag_index < 8) scale = 0.78f;
            else if (zigzag_index < 24) scale = 0.74f;
            else scale = 0.86f;
        }

        // Gentle color ramps live in the low-frequency chroma coefficients
        // (DC + first ACs); below q90 their steps were coarse enough to
        // flatten a smooth ramp into visible bands, so refine them 2x --
        // but only when the encoder asked for it (smooth-ramp tiles), never
        // as a blanket change: it costs bytes and can flip RDO modes on
        // textured content.
        if (refine_low_frequencies_ && quality_ < 90.0f && zigzag_index < 8) {
            scale *= 0.5f;
        }
        return scale;
    }

    [[nodiscard]] float deadzone_scale(int zigzag_index) const {
        if (zigzag_index == 0) {
            return is_chroma_ ? 0.44f : 0.5f;
        }

        float scale = is_chroma_ ? 0.46f : 0.52f;
        if (quality_ >= 85.0f) {
            scale += is_chroma_ ? 0.02f : 0.03f;
        } else if (quality_ >= 75.0f) {
            scale += is_chroma_ ? 0.0f : 0.01f;
        }

        if (zigzag_index >= 20) {
            scale += is_chroma_ ? 0.035f : 0.06f;
        } else if (zigzag_index >= 8) {
            scale += is_chroma_ ? 0.02f : 0.03f;
        }

        return std::clamp(scale, is_chroma_ ? 0.42f : 0.5f, 0.69f);
    }

    std::array<uint16_t, 64> table_{};
    std::array<float, 64>    inv_table_{};
    float                    quality_ = 75.0f;
    bool                     is_chroma_ = false;
    uint8_t                  bit_depth_ = 8;
    bool                     refine_low_frequencies_ = false;
};

inline int quality_to_qp(float quality) {
    return static_cast<int>(100.0f - std::clamp(quality, 0.0f, 100.0f));
}

inline float qp_to_lambda(int qp, uint8_t bit_depth = 8) {
    const float q = static_cast<float>(qp);
    float lambda = 0.85f * std::pow(2.0f, (q - 12.0f) / 3.0f);
    if (bit_depth > 8) {
        // SSD grows with the square of the code-value range, so the
        // RDO lambda must grow by (2^(bd-8))^2 to keep the same
        // operating point: x16 at 10-bit, x256 at 12-bit (measured
        // slice 0, 2026-11-17: x16 reproduces the 8-bit operating
        // point on 17/24 q70-q80 combinations; x1 spends ~15% more
        // bits for +0.5 dB).
        lambda *= std::pow(4.0f, static_cast<float>(bit_depth - 8));
    }
    return lambda;
}

}
