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
#include <gtest/gtest.h>
#include "../src/core/quantize.h"

using namespace wk;

TEST(QuantizeTest, UsesNaturalMatrixInZigzagOrder) {
    QuantTable quant;
    quant.build(50.0f, false, 8);

    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        // The base table is capped at kMaxBaseQuantValue (q90-anomaly fix,
        // 2026-11-13): steps are min(base, cap), zigzag order unchanged.
        const uint16_t expected = std::min<uint16_t>(
            BASE_QUANT_LUMA[ZIGZAG_ORDER[i]],
            static_cast<uint16_t>(kMaxBaseQuantValue));
        EXPECT_EQ(steps[i], expected)
            << "Unexpected quant step at zigzag index " << i;
    }
}

TEST(QuantizeTest, BitDepthScalingPreservesZigzagMapping) {
    QuantTable quant;
    quant.build(50.0f, false, 10);

    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        // Capped base (see above) times the 10-bit multiplier 4.
        const uint16_t expected = static_cast<uint16_t>(
            std::min<uint16_t>(BASE_QUANT_LUMA[ZIGZAG_ORDER[i]],
                               static_cast<uint16_t>(kMaxBaseQuantValue)) *
            4);
        EXPECT_EQ(steps[i], expected)
            << "Unexpected 10-bit quant step at zigzag index " << i;
    }
}

// The low-frequency chroma refinement is opt-in per tile (the encoder only
// enables it for smooth chroma ramps). Default: the historical steps.
TEST(QuantizeTest, ChromaLowFrequencyRefinementIsOptIn) {
    QuantTable quant;
    quant.build(50.0f, true, 8);
    EXPECT_EQ(quant.steps()[0], 12);
    EXPECT_EQ(quant.steps()[1], 12);

    quant.build(50.0f, true, 8, true);
    const uint16_t expected_low[8] = {6, 6, 6, 7, 7, 7, 10, 8};
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(quant.steps()[i], expected_low[i])
            << "Unexpected refined chroma quant step at zigzag index " << i;
    }
    // zigzag 8+ unchanged: 21 * 0.74 = 15, and 46 is capped to 40 first
    // (kMaxBaseQuantValue), so 40 * 0.86 = 34.
    EXPECT_EQ(quant.steps()[8], 15);
    EXPECT_EQ(quant.steps()[24], 34);

    // q>=90: the refinement never applies, even when requested.
    quant.build(90.0f, true, 8, true);
    EXPECT_EQ(quant.steps()[0], 1);
    EXPECT_EQ(quant.steps()[1], 1);
}

// Adaptive quantization (layout V7): per-block step multipliers. Index 4
// is the identity; every factor is exactly representable in binary
// floating point so encoder and decoder agree bit-for-bit.
TEST(QuantizeTest, QuantScaleTableMatchesSpec) {
    EXPECT_EQ(QuantTable::kQuantScaleFactors.size(), 8u);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[0], 0.5f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[1], 0.625f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[2], 0.75f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[3], 0.875f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[4], 1.0f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[5], 1.25f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[6], 1.5f);
    EXPECT_FLOAT_EQ(QuantTable::kQuantScaleFactors[7], 2.0f);
    EXPECT_EQ(QuantTable::kQuantScaleIdentity, 4u);
}

// Scale 1.0 reproduces the plain quantize/dequantize bit-for-bit.
TEST(QuantizeTest, ScaledQuantizeMatchesUnscaledAtIdentity) {
    QuantTable quant;
    quant.build(50.0f, false, 8);

    DctBlock input{};
    for (int i = 0; i < 64; ++i) {
        input[i] = static_cast<float>((i * 37) % 61) - 30.0f;
    }
    DctBlockI16 plain{}, scaled{};
    quant.quantize(input, plain);
    quant.quantize_scaled(input, scaled, 1.0f);
    EXPECT_EQ(plain, scaled);

    DctBlock plain_back{}, scaled_back{};
    quant.dequantize(plain, plain_back);
    quant.dequantize_scaled(plain, scaled_back, 1.0f);
    EXPECT_EQ(plain_back, scaled_back);
}

// Chroma HF deadzone (#k1): a quantized chroma coefficient with zigzag
// index >= 24 and |q| <= 1 is zeroed; luma, lower zigzag positions, and
// |q| >= 2 are untouched. The rule lives in the quantizer, so RDO and
// reconstruction see the zeroed coefficients.
TEST(QuantizeTest, ChromaHighFrequencyDeadzone) {
    QuantTable chroma;
    chroma.build(75.0f, true, 8);

    // Zigzag 24, |q| = 1 -> 0 (both signs, both quantize paths).
    for (float sign : {1.0f, -1.0f}) {
        DctBlock input{};
        input[ZIGZAG_ORDER[24]] = sign * static_cast<float>(chroma.steps()[24]);
        DctBlockI16 out{}, out_scaled{};
        chroma.quantize(input, out);
        chroma.quantize_scaled(input, out_scaled, 1.0f);
        EXPECT_EQ(out[24], 0) << "chroma zigzag 24 q=1 should be zeroed (sign " << sign << ")";
        EXPECT_EQ(out_scaled[24], 0) << "scaled chroma zigzag 24 q=1 should be zeroed (sign " << sign << ")";
    }

    // Zigzag 63 (highest), |q| = 1 -> 0.
    {
        DctBlock input{};
        input[ZIGZAG_ORDER[63]] = static_cast<float>(chroma.steps()[63]);
        DctBlockI16 out{};
        chroma.quantize(input, out);
        EXPECT_EQ(out[63], 0);
    }

    // Zigzag 23 (just below the threshold), |q| = 1 -> untouched.
    {
        DctBlock input{};
        input[ZIGZAG_ORDER[23]] = static_cast<float>(chroma.steps()[23]);
        DctBlockI16 out{};
        chroma.quantize(input, out);
        EXPECT_EQ(out[23], 1);
    }

    // Zigzag 24, |q| = 2 -> untouched.
    {
        DctBlock input{};
        input[ZIGZAG_ORDER[24]] = 2.0f * static_cast<float>(chroma.steps()[24]);
        DctBlockI16 out{};
        chroma.quantize(input, out);
        EXPECT_EQ(out[24], 2);
        // With scale 0.5 the effective step halves, so v = step still
        // quantizes to 2 and survives the deadzone.
        DctBlock scaled_input{};
        scaled_input[ZIGZAG_ORDER[24]] = static_cast<float>(chroma.steps()[24]);
        DctBlockI16 out_scaled{};
        chroma.quantize_scaled(scaled_input, out_scaled, 0.5f);
        EXPECT_EQ(out_scaled[24], 2);
    }

    // DC, |q| = 1 -> untouched (below the threshold).
    {
        DctBlock input{};
        input[ZIGZAG_ORDER[0]] = static_cast<float>(chroma.steps()[0]);
        DctBlockI16 out{};
        chroma.quantize(input, out);
        EXPECT_EQ(out[0], 1);
    }

    // Luma at zigzag 24, |q| = 1 -> untouched (chroma only).
    QuantTable luma;
    luma.build(75.0f, false, 8);
    {
        DctBlock input{};
        input[ZIGZAG_ORDER[24]] = static_cast<float>(luma.steps()[24]);
        DctBlockI16 out{}, out_scaled{};
        luma.quantize(input, out);
        luma.quantize_scaled(input, out_scaled, 1.0f);
        EXPECT_EQ(out[24], 1);
        EXPECT_EQ(out_scaled[24], 1);
    }

    // Scaled chroma path with a halved step: v = step/2 quantizes to 1
    // at zigzag 24 and is zeroed.
    {
        DctBlock input{};
        input[ZIGZAG_ORDER[24]] = 0.5f * static_cast<float>(chroma.steps()[24]);
        DctBlockI16 out{};
        chroma.quantize_scaled(input, out, 0.5f);
        EXPECT_EQ(out[24], 0);
    }
}

// The deadzone is 8-bit only: at 10/12-bit the |q| = 1 chroma HF
// coefficients are signal (gradient textures lose ~2.2 dB when they
// are zeroed), so higher bit depths keep the old behavior.
TEST(QuantizeTest, ChromaHighFrequencyDeadzoneIs8BitOnly) {
    for (uint8_t bit_depth : {10, 12}) {
        QuantTable chroma;
        chroma.build(75.0f, true, bit_depth);
        DctBlock input{};
        input[ZIGZAG_ORDER[24]] = static_cast<float>(chroma.steps()[24]);
        DctBlockI16 out{}, out_scaled{};
        chroma.quantize(input, out);
        chroma.quantize_scaled(input, out_scaled, 1.0f);
        EXPECT_EQ(out[24], 1) << "10/12-bit chroma zigzag 24 q=1 must survive";
        EXPECT_EQ(out_scaled[24], 1) << "10/12-bit scaled chroma zigzag 24 q=1 must survive";
    }
}
// survives when the block's step is halved: this is the mechanism that
// lets a seed row keep its slope. (Luma q50: DC step 16, AC1 step 11.)
TEST(QuantizeTest, ScaledQuantizeKeepsSmallSlopeCoefficient) {
    QuantTable quant;
    quant.build(50.0f, false, 8);

    DctBlock input{};
    input[ZIGZAG_ORDER[1]] = 5.0f;  // AC1, below the 0.52*11 deadzone
    DctBlockI16 plain{}, scaled{};
    quant.quantize(input, plain);
    quant.quantize_scaled(input, scaled, 0.5f);
    EXPECT_EQ(plain[1], 0);
    EXPECT_EQ(scaled[1], 1);

    DctBlock back{};
    quant.dequantize_scaled(scaled, back, 0.5f);
    EXPECT_FLOAT_EQ(back[ZIGZAG_ORDER[1]], 5.5f);
}
