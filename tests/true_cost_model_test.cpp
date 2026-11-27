// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "../src/core/true_cost_model.h"
#include <cmath>

using namespace wk;

namespace {

// 4 blocks; spans are the blocks' own spans (as the encoder computes them):
//   b0: [3, 1, ...] span 2, mode DC
//   b1: [3, 0, ...] span 1, mode DC
//   b2: [0, 0, ...] span 0, mode V
//   b3: [2, 0, ...] span 1, mode V
TrueCostModel SmallModel() {
    std::vector<PredMode> modes{PredMode::DC, PredMode::DC, PredMode::V, PredMode::V};
    std::vector<uint8_t> spans{2, 1, 0, 1};
    std::vector<DctBlockI16> blocks(4);
    for (auto& b : blocks) b.fill(0);
    blocks[0][0] = 3;
    blocks[0][1] = 1;
    blocks[1][0] = 3;
    blocks[3][0] = 2;
    TrueCostModel m;
    m.build(modes, spans, blocks);
    return m;
}

DctBlockI16 BlockWith(int16_t v0, int16_t v1 = 0) {
    DctBlockI16 b{};
    b.fill(0);
    b[0] = v0;
    b[1] = v1;
    return b;
}

}  // namespace

TEST(TrueCostModelTest, ModeCostFromFrequencies) {
    TrueCostModel m = SmallModel();
    // DC and V each occur 2/4 -> -log2(0.5) = 1 bit.
    EXPECT_FLOAT_EQ(m.mode_cost[static_cast<int>(PredMode::DC)], 1.0f);
    EXPECT_FLOAT_EQ(m.mode_cost[static_cast<int>(PredMode::V)], 1.0f);
    // Unseen mode costs -log2(0.5/4) = 3 bits.
    EXPECT_FLOAT_EQ(m.mode_cost[static_cast<int>(PredMode::H)], 3.0f);
}

TEST(TrueCostModelTest, SpanCostFromFrequencies) {
    TrueCostModel m = SmallModel();
    // span 1 occurs 2/4 -> 1 bit; span 2 and span 0 each 1/4 -> 2 bits.
    EXPECT_FLOAT_EQ(m.span_cost[1], 1.0f);
    EXPECT_FLOAT_EQ(m.span_cost[2], 2.0f);
    EXPECT_FLOAT_EQ(m.span_cost[0], 2.0f);
    // Unseen span costs -log2(0.5/4) = 3 bits.
    EXPECT_FLOAT_EQ(m.span_cost[7], 3.0f);
}

TEST(TrueCostModelTest, CoeffBitsMagnitudePlusSign) {
    TrueCostModel m = SmallModel();
    // q=[3,1], span 2: pos0 |3| is 2/3 of active -> -log2(2/3), +1 sign;
    // pos1 |1| is 1/1 of active -> 0, +1 sign.
    EXPECT_NEAR(m.coeff_bits(BlockWith(3, 1), 2),
                -std::log2(2.0 / 3.0) + 2.0f, 1e-5);
    // q=[3,0], span 1: only position 0 is priced.
    EXPECT_NEAR(m.coeff_bits(BlockWith(3), 1),
                -std::log2(2.0 / 3.0) + 1.0f, 1e-5);
}

TEST(TrueCostModelTest, CoeffBitsUnseenMagnitudeIsSmoothed) {
    TrueCostModel m = SmallModel();
    // |5| never occurs at pos 0 (active=3) -> -log2(0.5/3), +1 sign;
    // pos1 |0| never occurs (active=1) -> -log2(0.5/1) = 1.
    EXPECT_NEAR(m.coeff_bits(BlockWith(5), 2),
                -std::log2(0.5 / 3.0) + 1.0f + 1.0f, 1e-5);
}

TEST(TrueCostModelTest, BlockBitsSumsModeSpanAndCoeff) {
    TrueCostModel m = SmallModel();
    // q=[3,1] has span 2: mode DC 1 + span 2 (2) + coeff (-log2(2/3) + 2).
    EXPECT_NEAR(m.block_bits(BlockWith(3, 1), PredMode::DC),
                1.0f + 2.0f - static_cast<float>(std::log2(2.0 / 3.0)) + 2.0f, 1e-5);
    // All-zero block: span 0 (2 bits) + mode V 1 bit, no coeff bits.
    EXPECT_FLOAT_EQ(m.block_bits(BlockWith(0), PredMode::V), 3.0f);
}

TEST(TrueCostModelTest, CoeffBitsHonoursExplicitSpan) {
    TrueCostModel m = SmallModel();
    // Span 0 prices nothing, whatever the coefficients are.
    EXPECT_FLOAT_EQ(m.coeff_bits(BlockWith(3, 1), 0), 0.0f);
}

TEST(TrueCostModelTest, EmptyModelStaysFinite) {
    TrueCostModel m;
    m.build({}, {}, {});
    const DctBlockI16 q = BlockWith(3);
    EXPECT_TRUE(std::isfinite(m.block_bits(q, PredMode::DC)));
    EXPECT_TRUE(std::isfinite(m.coeff_bits(q, 2)));
}

namespace {

// #rdo (iterasi 1): 4 blocks, span 4, so positions 1-3 (one band) are all
// active. Position 1 never sees |2|; positions 2-3 do (band total:
// |0|x7, |1|x3, |2|x2 over 12 pairs). At pos 1: p_emp(0)=1/4, p_emp(1)=3/4.
TrueCostModel BandModel(float shrink_beta) {
    std::vector<PredMode> modes(4, PredMode::DC);
    std::vector<uint8_t> spans{4, 4, 4, 4};
    std::vector<DctBlockI16> blocks(4);
    for (auto& b : blocks) b.fill(0);
    blocks[0][1] = 1;
    blocks[1][1] = 1;
    blocks[1][2] = 2;
    blocks[2][1] = 0;
    blocks[2][3] = 2;
    blocks[3][1] = 1;
    TrueCostModel m;
    m.build(modes, spans, blocks, shrink_beta);
    return m;
}

}  // namespace

TEST(TrueCostModelTest, ShrinkBlendsPositionWithBandPrior) {
    TrueCostModel m = BandModel(0.5f);
    // Pos 1, |1|: (1-0.5)*3/4 + 0.5*3/12 = 0.5 -> 1 bit (was 0.415 at beta 0).
    EXPECT_NEAR(m.mag_cost[1][1], 1.0f, 1e-5);
    // Pos 1, |0|: 0.5*1/4 + 0.5*7/12 = 5/12 -> -log2(5/12).
    EXPECT_NEAR(m.mag_cost[1][0], -std::log2(5.0 / 12.0), 1e-5);
    // Pos 1, |2|: never seen at this position, seen in the band:
    // 0.5*0 + 0.5*2/12 = 1/12 -> -log2(1/12), NOT the unseen cost.
    ASSERT_GE(m.mag_cost[1].size(), 3u);
    EXPECT_NEAR(m.mag_cost[1][2], -std::log2(1.0 / 12.0), 1e-5);
}

TEST(TrueCostModelTest, ShrinkLeavesDcBandUnchanged) {
    // Band {0} is just position 0, so its prior is itself: beta must not
    // change DC costs at all.
    TrueCostModel plain = BandModel(0.0f);
    TrueCostModel shrunk = BandModel(0.5f);
    ASSERT_EQ(plain.mag_cost[0].size(), shrunk.mag_cost[0].size());
    for (size_t v = 0; v < plain.mag_cost[0].size(); ++v) {
        EXPECT_FLOAT_EQ(shrunk.mag_cost[0][v], plain.mag_cost[0][v]);
    }
    // And beta 0 reproduces the unshrunk model everywhere: same tables
    // as a second beta-0 build.
    TrueCostModel plain2 = BandModel(0.0f);
    for (int pos = 0; pos < 64; ++pos) {
        ASSERT_EQ(plain.mag_cost[pos].size(), plain2.mag_cost[pos].size());
        for (size_t v = 0; v < plain.mag_cost[pos].size(); ++v) {
            EXPECT_FLOAT_EQ(plain2.mag_cost[pos][v], plain.mag_cost[pos][v]);
        }
    }
}

TEST(TrueCostModelTest, ModeSpanShrinkBlendsTowardUniform) {
    // SmallModel: DC and V each 2/4, H unseen; spans 0/1/2 as 1/2/1 of 4.
    std::vector<PredMode> modes{PredMode::DC, PredMode::DC, PredMode::V, PredMode::V};
    std::vector<uint8_t> spans{2, 1, 0, 1};
    std::vector<DctBlockI16> blocks(4);
    for (auto& b : blocks) b.fill(0);
    TrueCostModel m;
    m.build(modes, spans, blocks, 0.0f, 0.5f);
    // DC: 0.5*0.5 + 0.5*(1/15) = 0.2833 -> -log2.
    EXPECT_NEAR(m.mode_cost[static_cast<int>(PredMode::DC)],
                -std::log2(0.5f * 0.5f + 0.5f * (1.0f / 15.0f)), 1e-5);
    // H (unseen): 0.5*0 + 0.5*(1/15) = 1/30 -> -log2(1/30).
    EXPECT_NEAR(m.mode_cost[static_cast<int>(PredMode::H)],
                -std::log2(1.0f / 30.0f), 1e-5);
    // Span 1 (2/4): 0.5*0.5 + 0.5*(1/65).
    EXPECT_NEAR(m.span_cost[1],
                -std::log2(0.5f * 0.5f + 0.5f * (1.0f / 65.0f)), 1e-5);
    // Span 7 (unseen): 0.5*(1/65).
    EXPECT_NEAR(m.span_cost[7], -std::log2(0.5f * (1.0f / 65.0f)), 1e-5);
    // Beta 0 for mode/span reproduces v1 exactly.
    TrueCostModel v1;
    v1.build(modes, spans, blocks);
    EXPECT_FLOAT_EQ(v1.mode_cost[static_cast<int>(PredMode::DC)], 1.0f);
    EXPECT_FLOAT_EQ(v1.mode_cost[static_cast<int>(PredMode::H)], 3.0f);
    EXPECT_FLOAT_EQ(v1.span_cost[7], 3.0f);
}

TEST(TrueCostModelTest, DcShrinkBlendsTowardFlatPrior) {
    // 4 blocks, DC magnitudes {0,0,0,5}: p_emp(0)=3/4, p_emp(5)=1/4,
    // cap 5 -> flat prior 1/6 per value. dc_beta 0.5:
    // p'(0) = 0.5*3/4 + 0.5*1/6 = 11/24; p'(5) = 0.5*1/4 + 0.5*1/6 = 5/24;
    // p'(3) (unseen) = 0.5*1/6 = 1/12.
    std::vector<PredMode> modes(4, PredMode::DC);
    std::vector<uint8_t> spans{1, 1, 1, 1};
    std::vector<DctBlockI16> blocks(4);
    for (auto& b : blocks) b.fill(0);
    blocks[3][0] = 5;
    TrueCostModel m;
    m.build(modes, spans, blocks, 0.0f, 0.0f, 0.5f);
    ASSERT_GE(m.mag_cost[0].size(), 6u);
    EXPECT_NEAR(m.mag_cost[0][0], -std::log2(11.0 / 24.0), 1e-5);
    EXPECT_NEAR(m.mag_cost[0][5], -std::log2(5.0 / 24.0), 1e-5);
    EXPECT_NEAR(m.mag_cost[0][3], -std::log2(1.0 / 12.0), 1e-5);
    // AC positions are untouched by dc_beta.
    TrueCostModel plain;
    plain.build(modes, spans, blocks);
    EXPECT_FLOAT_EQ(m.mag_cost[1][0], plain.mag_cost[1][0]);
}
