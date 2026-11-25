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

    // The base table is capped at kMaxBaseQuantValue (q90-anomaly fix,
    // 2026-11-13): steps are min(base, cap), zigzag order unchanged,
    // then reshaped by the luma quant-table shape (#qtab): the pass-2
    // table rescaled by the per-position ratio to nominal q75.
    const std::array<uint16_t, 64> expected = {{
        16, 11, 12, 16, 16, 12, 20, 18,
        17, 18, 22, 21, 20, 23, 30, 50,
        20, 20, 18, 18, 20, 32, 29, 29,
        23, 32, 32, 32, 32, 32, 32, 32,
        28, 28, 28, 28, 28, 28, 28, 28,
        28, 28, 28, 28, 28, 28, 28, 28,
        28, 28, 28, 28, 28, 28, 28, 28,
        28, 28, 28, 28, 28, 28, 28, 28}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected quant step at zigzag index " << i;
    }
}

TEST(QuantizeTest, BitDepthScalingPreservesZigzagMapping) {
    QuantTable quant;
    quant.build(50.0f, false, 10);

    // Capped base (see above) times the 10-bit multiplier 4, then the
    // luma quant-table shape (#qtab) as in the 8-bit test.
    const std::array<uint16_t, 64> expected = {{
        64, 40, 48, 64, 64, 48, 80, 72,
        64, 72, 88, 80, 80, 88, 120, 200,
        80, 80, 72, 72, 80, 128, 112, 112,
        88, 128, 128, 128, 128, 128, 128, 128,
        112, 112, 112, 112, 112, 112, 112, 112,
        112, 112, 112, 112, 112, 112, 112, 112,
        112, 112, 112, 112, 112, 112, 112, 112,
        112, 112, 112, 112, 112, 112, 112, 112}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected 10-bit quant step at zigzag index " << i;
    }
}

// The low-frequency chroma refinement is opt-in per tile (the encoder only
// enables it for smooth chroma ramps). Default: the historical steps,
// reshaped by the chroma quant-table shape (#qtab, pass-2).
TEST(QuantizeTest, ChromaLowFrequencyRefinementIsOptIn) {
    QuantTable quant;
    quant.build(50.0f, true, 8);
    EXPECT_EQ(quant.steps()[0], 12);
    EXPECT_EQ(quant.steps()[1], 14);

    quant.build(50.0f, true, 8, true);
    const uint16_t expected_low[8] = {6, 7, 7, 9, 7, 7, 10, 8};
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

// Quant-table shape (#qtab, measured 2026-11-25 by encoder-side coordinate
// descent at q75, pass 2 with ramp constraints): the luma table is reshaped
// per zigzag band on top of the nominal table — Y{1..3} x1.08, Y{4..7} and
// Y{8..15} x1.25, Y{16..31} x0.8, Y{32..63} x0.7; the chroma table is
// reshaped too — C{1..3} x1.25, C{32..63} x0.8, the rest unchanged.
// Luma applies as a per-position ratio against the nominal q75 table
// (variant A); chroma applies as a multiplier on the nominal table at the
// tile's bit depth, so the q75 table below is reproduced exactly.
TEST(QuantizeTest, QuantTableShapePass2Q75Luma) {
    QuantTable quant;
    quant.build(75.0f, false, 8);
    const std::array<uint16_t, 64> expected = {{
        8, 5, 6, 8, 8, 6, 10, 9,
        8, 9, 11, 10, 10, 11, 15, 25,
        10, 10, 9, 9, 10, 16, 14, 14,
        11, 16, 16, 16, 16, 16, 16, 16,
        14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14,
        14, 14, 14, 14, 14, 14, 14, 14}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected shaped luma quant step at zigzag index " << i;
    }
}

// The shape covers chroma too: the q75 chroma table is the pass-2 table,
// not the nominal one (the pass-1 slice left chroma unchanged).
TEST(QuantizeTest, QuantTableShapePass2Q75Chroma) {
    QuantTable quant;
    quant.build(75.0f, true, 8);
    const std::array<uint16_t, 64> expected = {{
        5, 6, 6, 8, 6, 6, 8, 7,
        6, 8, 11, 9, 8, 9, 11, 12,
        12, 11, 11, 12, 12, 12, 12, 12,
        15, 15, 15, 15, 15, 15, 15, 15,
        12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected chroma quant step at zigzag index " << i;
    }
}

// The 10-bit q75 luma table is the pass-2 table times the 10-bit
// multiplier 4, exactly (#qtab scales the fit to the bit depth).
TEST(QuantizeTest, QuantTableShapePass2Q75Luma10Bit) {
    QuantTable quant;
    quant.build(75.0f, false, 10);
    const std::array<uint16_t, 64> expected = {{
        32, 20, 24, 32, 32, 24, 40, 36,
        32, 36, 44, 40, 40, 44, 60, 100,
        40, 40, 36, 36, 40, 64, 56, 56,
        44, 64, 64, 64, 64, 64, 64, 64,
        56, 56, 56, 56, 56, 56, 56, 56,
        56, 56, 56, 56, 56, 56, 56, 56,
        56, 56, 56, 56, 56, 56, 56, 56,
        56, 56, 56, 56, 56, 56, 56, 56}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected shaped 10-bit luma quant step at zigzag index " << i;
    }
}

// The 10-bit q75 chroma table is NOT the pass-2 table times 4: the
// nominal chroma table does not scale by exactly 4 (truncation), so the
// chroma multiplier is applied on the nominal 10-bit table instead.
// E.g. zigzag 0: lround(20 * 5/5) = 21, not 5 * 4 = 20.
TEST(QuantizeTest, QuantTableShapePass2Q75Chroma10Bit) {
    QuantTable quant;
    quant.build(75.0f, true, 10);
    const std::array<uint16_t, 64> expected = {{
        21, 25, 25, 33, 24, 25, 35, 28,
        26, 33, 46, 38, 33, 38, 46, 51,
        51, 46, 46, 51, 51, 51, 51, 51,
        60, 60, 60, 60, 60, 60, 60, 60,
        48, 48, 48, 48, 48, 48, 48, 48,
        48, 48, 48, 48, 48, 48, 48, 48,
        48, 48, 48, 48, 48, 48, 48, 48,
        48, 48, 48, 48, 48, 48, 48, 48}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected shaped 10-bit chroma quant step at zigzag index " << i;
    }
}

// A quality-adapted tile (ember's tile builds at q~77.7) must reproduce
// the measured pass-2 tile table exactly. This is what separates the
// #qtab ratio mechanism from plain band multipliers: at zigzag 4, 8
// the multipliers round to 6, the measured table has 7.
TEST(QuantizeTest, QuantTableShapeAdaptedQualityReproducesPass2Tile) {
    QuantTable quant;
    quant.build(77.7f, false, 8);

    const std::array<uint16_t, 64> expected = {{
        7, 4, 5, 7, 7, 5, 9, 8,
        7, 8, 10, 9, 9, 10, 13, 21,
        8, 8, 7, 7, 8, 14, 12, 12,
        9, 14, 14, 14, 14, 14, 14, 14,
        12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12,
        12, 12, 12, 12, 12, 12, 12, 12}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected shaped quant step at zigzag index " << i
            << " for the adapted-quality tile";
    }
}

// Same adapted-quality check for chroma: ember's tile builds its chroma
// table at q~81.8 (interval 81.78-81.90 reproduces the measured table;
// the diag's printed chroma_q=86.11 is derived from the DC step, not
// the build quality). The multiplier semantics must reproduce the
// measured pass-2 chroma tile table exactly.
TEST(QuantizeTest, QuantTableShapeAdaptedQualityReproducesPass2ChromaTile) {
    QuantTable quant;
    quant.build(81.8f, true, 8);

    const std::array<uint16_t, 64> expected = {{
        3, 4, 4, 5, 4, 4, 6, 5,
        4, 6, 8, 6, 6, 6, 8, 9,
        9, 8, 8, 9, 9, 9, 9, 9,
        11, 11, 11, 11, 11, 11, 11, 11,
        9, 9, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 9, 9, 9}};
    const auto& steps = quant.steps();
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(steps[i], expected[i])
            << "Unexpected shaped chroma quant step at zigzag index " << i
            << " for the adapted-quality tile";
    }
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
