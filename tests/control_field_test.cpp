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
// distributed under the Apache License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <gtest/gtest.h>

#include <cmath>

#include "../src/control_field.h"

using namespace wk;

namespace {

// Fills an 8x8 block region of a tile with a constant value.
void fill_block(std::vector<int16_t>& tile, uint32_t tw, uint32_t bx, uint32_t by, int16_t v) {
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c)
            tile[(by * 8 + r) * tw + (bx * 8 + c)] = v;
}

}  // namespace

TEST(ControlFieldTest, FlatImageIsAllFlat) {
    // 16x16 tile, all pixels 128: every block is smooth.
    // Flat blocks stay at identity quant (finer quant on flat blocks
    // was measured and rejected: it costs far more bits than the
    // coarser quant on textured blocks saves).
    std::vector<int16_t> tile(16 * 16, 128);
    auto field = analyze_control_field(tile.data(), 16, 16);
    ASSERT_EQ(field.size(), 4u);
    for (const auto& bc : field) {
        EXPECT_EQ(bc.content, BlockContent::Flat);
        EXPECT_EQ(bc.quant_scale_index, 4u);  // identity
    }
}

TEST(ControlFieldTest, NoisyImageIsTextured) {
    // 16x16 tile, alternating 0/255 checkerboard: high variance.
    std::vector<int16_t> tile(16 * 16, 0);
    for (uint32_t r = 0; r < 16; ++r)
        for (uint32_t c = 0; c < 16; ++c)
            tile[r * 16 + c] = ((r + c) % 2 == 0) ? 0 : 255;
    auto field = analyze_control_field(tile.data(), 16, 16);
    ASSERT_EQ(field.size(), 4u);
    for (const auto& bc : field) {
        EXPECT_EQ(bc.content, BlockContent::Textured);
        EXPECT_EQ(bc.quant_scale_index, 5u);  // 1.25, coarser
    }
}

TEST(ControlFieldTest, SharpEdgeIsEdge) {
    // 16x16 tile: a single 1-pixel-wide bright line (value 200) on a
    // flat 100 background. The two blocks straddling the line contain
    // a sharp step (max gradient 100, low mean gradient) and must read
    // as Edge, not Textured, even though the bimodal split also gives
    // them high variance. The untouched blocks are flat.
    std::vector<int16_t> tile(16 * 16, 100);
    for (uint32_t r = 0; r < 16; ++r) tile[r * 16 + 8] = 200;
    auto field = analyze_control_field(tile.data(), 16, 16);
    ASSERT_EQ(field.size(), 4u);
    EXPECT_EQ(field[1].content, BlockContent::Edge);
    EXPECT_EQ(field[3].content, BlockContent::Edge);
    EXPECT_EQ(field[1].quant_scale_index, 4u);  // identity
    EXPECT_EQ(field[0].content, BlockContent::Flat);
    EXPECT_EQ(field[2].content, BlockContent::Flat);
}

TEST(ControlFieldTest, SmoothGradientIsFlat) {
    // A gentle ramp (0..15 across 16 px) has low variance per block
    // and small gradients — it should read as Flat (banding-prone).
    std::vector<int16_t> tile(16 * 16, 0);
    for (uint32_t r = 0; r < 16; ++r)
        for (uint32_t c = 0; c < 16; ++c)
            tile[r * 16 + c] = static_cast<int16_t>(c);
    auto field = analyze_control_field(tile.data(), 16, 16);
    ASSERT_EQ(field.size(), 4u);
    for (const auto& bc : field) {
        EXPECT_EQ(bc.content, BlockContent::Flat);
    }
}
