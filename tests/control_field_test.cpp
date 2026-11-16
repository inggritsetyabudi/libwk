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

// ---- Content-type classes (chroma-aware classification) ----
// Each test below is a single 8x8 block tile (tw=th=8) with explicit
// Y/Cb/Cr values, hand-checked against the classification rules:
// means, variances and stddevs are exact for the patterns used, so
// the expected class follows from the rules, not from the diagnostic.

// Fills the single 8x8 block of an 8x8 tile with a constant (Y,Cb,Cr).
void fill_ycbcr_block(std::vector<int16_t>& y, std::vector<int16_t>& cb,
                      std::vector<int16_t>& cr,
                      int16_t yv, int16_t cbv, int16_t crv) {
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) {
            y[r * 8 + c] = yv;
            cb[r * 8 + c] = cbv;
            cr[r * 8 + c] = crv;
        }
}

// Fills the single 8x8 block with a checkerboard of two (Y,Cb,Cr)
// values, alternating by (r+c) parity.
void fill_ycbcr_checker(std::vector<int16_t>& y, std::vector<int16_t>& cb,
                        std::vector<int16_t>& cr,
                        int16_t y0, int16_t cb0, int16_t cr0,
                        int16_t y1, int16_t cb1, int16_t cr1) {
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) {
            const bool alt = ((r + c) % 2 == 0);
            y[r * 8 + c] = alt ? y0 : y1;
            cb[r * 8 + c] = alt ? cb0 : cb1;
            cr[r * 8 + c] = alt ? cr0 : cr1;
        }
}

TEST(ControlFieldTest, SkinBlockIsSkin) {
    // Skin-tone checkerboard: means Y=150, Cb=115, Cr=145, hue -37 deg
    // (inside the skin box), chroma std 5 (passes the uniformity
    // gate). Luma is uniform, so the v1 class is Flat (quant 4) — the
    // content class is Skin while the quant suggestion stays v1's.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_checker(y, cb, cr, 150, 110, 140, 150, 120, 150);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Skin);
    EXPECT_EQ(field[0].quant_scale_index, 4u);
}

TEST(ControlFieldTest, SkyBlockIsSky) {
    // Uniform bright blue: Cb=145, Cr=110, Y=180, var 0. Inside the
    // sky box; too blue/dark in Cr to be skin.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_block(y, cb, cr, 180, 145, 110);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Sky);
    EXPECT_EQ(field[0].quant_scale_index, 4u);
}

TEST(ControlFieldTest, FoliageBlockIsFoliage) {
    // Green, saturated, textured: Cb=60, Cr=135, hue -84 deg, sat 68,
    // Y checkerboard 90/110 (var 100). Too green in Cb to be skin.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_checker(y, cb, cr, 90, 60, 135, 110, 60, 135);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Foliage);
    EXPECT_EQ(field[0].quant_scale_index, 4u);  // v1: Normal
}

TEST(ControlFieldTest, TextUiBlockIsTextUi) {
    // Sharp luma step (50 -> 200, max grad 150, mean grad ~10.7) on
    // flat neutral chroma (Cb=Cr=128): concentrated edge, not skin
    // (Cr outside the skin box), not texture (mean grad < 12).
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) y[r * 8 + c] = (c < 4) ? 50 : 200;
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::TextUI);
    EXPECT_EQ(field[0].quant_scale_index, 4u);  // v1: Edge
}

TEST(ControlFieldTest, FlatGraphicsBlockIsFlatGraphics) {
    // Smooth vivid flat colour: Y=100, Cb=180, Cr=100, var 0,
    // sat ~59. Not sky (Y < 140), not foliage (Cb > 110).
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_block(y, cb, cr, 100, 180, 100);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::FlatGraphics);
    EXPECT_EQ(field[0].quant_scale_index, 4u);
}

TEST(ControlFieldTest, SkinWinsOverTextUi) {
    // Passes BOTH the skin box and the TextUI rule: sharp Y step
    // (50 -> 200) with skin-tone checkerboard chroma (means Cb=115,
    // Cr=145, std 5). Priority is Skin first, so the block reads as
    // Skin, not TextUI.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) {
            const bool alt = ((r + c) % 2 == 0);
            y[r * 8 + c] = (c < 4) ? 50 : 200;
            cb[r * 8 + c] = alt ? 110 : 120;
            cr[r * 8 + c] = alt ? 140 : 150;
        }
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Skin);
    EXPECT_EQ(field[0].quant_scale_index, 4u);  // v1: Edge
}

TEST(ControlFieldTest, UniformBeigeWallIsNotSkin) {
    // Perfectly uniform beige (Y=150, Cb=120, Cr=134): inside the
    // skin colour box and hue, but chroma std is 0, so the uniformity
    // gate rejects it. Falls back to v1 Flat — the same clean proof
    // as the beige-wall synthetic image in the colour-field research.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_block(y, cb, cr, 150, 120, 134);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Flat);
    EXPECT_EQ(field[0].quant_scale_index, 4u);
}

TEST(ControlFieldTest, TexturedFallbackStaysTextured) {
    // High-variance luma checkerboard (0/255) on neutral chroma: no
    // colour rule fires (Cr=128 outside the skin box, Cb=128 below
    // the sky gate and above the foliage gate), so the v1 class
    // Textured stands, with its coarser quant suggestion.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_checker(y, cb, cr, 0, 128, 128, 255, 128, 128);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Textured);
    EXPECT_EQ(field[0].quant_scale_index, 5u);
}

TEST(ControlFieldTest, NormalFallbackStaysNormal) {
    // Mild luma checkerboard (120/136, var 64) on neutral chroma:
    // no colour rule fires and v1 reads Normal.
    std::vector<int16_t> y(64, 0), cb(64, 128), cr(64, 128);
    fill_ycbcr_checker(y, cb, cr, 120, 128, 128, 136, 128, 128);
    auto field = analyze_control_field(y.data(), 8, 8, cb.data(), cr.data());
    ASSERT_EQ(field.size(), 1u);
    EXPECT_EQ(field[0].content, BlockContent::Normal);
    EXPECT_EQ(field[0].quant_scale_index, 4u);
}

TEST(ControlFieldTest, NullChromaBehavesAsLumaOnly) {
    // Passing null chroma planes is exactly the old luma-only call:
    // the checkerboard reads Textured (quant 5) and a flat tile
    // reads Flat (quant 4), as before.
    std::vector<int16_t> tile(16 * 16, 0);
    for (uint32_t r = 0; r < 16; ++r)
        for (uint32_t c = 0; c < 16; ++c)
            tile[r * 16 + c] = ((r + c) % 2 == 0) ? 0 : 255;
    auto field = analyze_control_field(tile.data(), 16, 16, nullptr, nullptr);
    ASSERT_EQ(field.size(), 4u);
    for (const auto& bc : field) {
        EXPECT_EQ(bc.content, BlockContent::Textured);
        EXPECT_EQ(bc.quant_scale_index, 5u);
    }
    std::vector<int16_t> flat(16 * 16, 128);
    field = analyze_control_field(flat.data(), 16, 16, nullptr, nullptr);
    ASSERT_EQ(field.size(), 4u);
    for (const auto& bc : field) {
        EXPECT_EQ(bc.content, BlockContent::Flat);
        EXPECT_EQ(bc.quant_scale_index, 4u);
    }
}
