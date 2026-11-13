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

    void build(float quality, bool is_chroma, uint8_t bit_depth,
              bool refine_low_frequencies = false) {
        quality_ = std::clamp(quality, 1.0f, 100.0f);
        is_chroma_ = is_chroma;
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
            if (is_chroma_) {
                base_value *= chroma_step_scale(i);
            }
            const float v = (base_value * scale / 100.0f) * bd_mul;
            table_[i] = static_cast<uint16_t>(std::max(v, 1.0f));
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
    bool                     refine_low_frequencies_ = false;
};

inline int quality_to_qp(float quality) {
    return static_cast<int>(100.0f - std::clamp(quality, 0.0f, 100.0f));
}

inline float qp_to_lambda(int qp) {
    const float q = static_cast<float>(qp);
    return 0.85f * std::pow(2.0f, (q - 12.0f) / 3.0f);
}

}
