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
#include "../src/quantize.h"

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

// A small low-frequency coefficient that dies in the q50 deadzone
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
