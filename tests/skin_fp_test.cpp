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

#include <array>
#include <cmath>
#include <vector>

#include <wk/wk.hpp>

#include "../src/colorspace.h"
#include "../src/control_field.h"
#include "../src/lossy_tile_encode.h"

using namespace wk;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Fabricates a per-block field: every block Normal except the Skin
// rectangles listed (x0, y0, w, h), which get the given chroma_grad.
std::vector<BlockControl> make_field(uint32_t bpr, uint32_t bpc,
                                     const std::vector<std::array<int, 4>>& skin_rects,
                                     float chroma_grad = 0.0f) {
    std::vector<BlockControl> field(static_cast<size_t>(bpr) * bpc);
    for (const auto& r : skin_rects) {
        for (int y = r[1]; y < r[1] + r[3]; ++y)
            for (int x = r[0]; x < r[0] + r[2]; ++x) {
                BlockControl& bc = field[static_cast<size_t>(y) * bpr + x];
                bc.content = BlockContent::Skin;
                bc.chroma_grad = chroma_grad;
            }
    }
    return field;
}

long count_dropped(const std::vector<uint8_t>& mask) {
    long n = 0;
    for (uint8_t b : mask) n += b ? 1 : 0;
    return n;
}

// Fills one 8x8 block of a 16x16 plane with a 2x2 checkerboard
// (v0 on even cells, v1 on odd cells).
void fill_checker_16(std::vector<int16_t>& plane, uint32_t bx, uint32_t by,
                     int16_t v0, int16_t v1) {
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c)
            plane[(by * 8 + r) * 16 + (bx * 8 + c)] =
                ((r + c) % 2 == 0) ? v0 : v1;
}

// Smooth low-frequency chroma (sine over 16 px): per-block chroma std
// stays above the uniformity gate while the chroma gradient stays
// small — the way real skin behaves (cgrad p50 0.91 in the research).
void fill_sine_chroma_16(std::vector<int16_t>& cb, std::vector<int16_t>& cr) {
    for (uint32_t r = 0; r < 16; ++r)
        for (uint32_t c = 0; c < 16; ++c) {
            cb[r * 16 + c] = static_cast<int16_t>(
                std::lround(112.0 + 4.0 * std::sin(2.0 * kPi * c / 16.0)));
            cr[r * 16 + c] = static_cast<int16_t>(
                std::lround(145.0 + 4.0 * std::cos(2.0 * kPi * r / 16.0)));
        }
}

}  // namespace

TEST(SkinFpTest, WideFlatSkinComponentIsDropped) {
    // A 50x10 Skin component (aspect 5.0, like the two noctis giants)
    // is dropped; a small 3x3 Skin patch elsewhere is kept.
    auto field = make_field(64, 16, {{0, 0, 50, 10}, {55, 12, 3, 3}});
    auto mask = skin_false_positive_mask(field, 64, 16);
    ASSERT_EQ(mask.size(), 64u * 16u);
    EXPECT_EQ(count_dropped(mask), 500);
    EXPECT_EQ(mask[0], 1);
    EXPECT_EQ(mask[9 * 64 + 49], 1);
    EXPECT_EQ(mask[12 * 64 + 55], 0);
    EXPECT_EQ(mask[14 * 64 + 57], 0);
}

TEST(SkinFpTest, NarrowOrTallSkinComponentIsKept) {
    // 30x13 (aspect 2.31, like the solis hands) is kept: width < 40.
    auto field_a = make_field(64, 16, {{0, 0, 30, 13}});
    EXPECT_EQ(count_dropped(skin_false_positive_mask(field_a, 64, 16)), 0);
    // 50x30 (aspect 1.67) is kept: aspect < 2.0.
    auto field_b = make_field(64, 32, {{0, 0, 50, 30}});
    EXPECT_EQ(count_dropped(skin_false_positive_mask(field_b, 64, 32)), 0);
}

TEST(SkinFpTest, HighChromaGradSkinBlockIsDropped) {
    // One Skin block, chroma_grad 3.5 (>= 3.0): dropped.
    {
        auto field = make_field(4, 4, {{1, 1, 1, 1}}, 3.5f);
        auto mask = skin_false_positive_mask(field, 4, 4);
        EXPECT_EQ(mask[1 * 4 + 1], 1);
    }
    // Same block, chroma_grad 2.9: kept.
    {
        auto field = make_field(4, 4, {{1, 1, 1, 1}}, 2.9f);
        auto mask = skin_false_positive_mask(field, 4, 4);
        EXPECT_EQ(mask[1 * 4 + 1], 0);
    }
    // Boundary: chroma_grad exactly 3.0 is dropped (>=).
    {
        auto field = make_field(4, 4, {{1, 1, 1, 1}}, 3.0f);
        auto mask = skin_false_positive_mask(field, 4, 4);
        EXPECT_EQ(mask[1 * 4 + 1], 1);
    }
    // A non-Skin block is never dropped, whatever its chroma_grad.
    {
        auto field = make_field(4, 4, {}, 10.0f);
        auto mask = skin_false_positive_mask(field, 4, 4);
        EXPECT_EQ(count_dropped(mask), 0);
    }
}

TEST(SkinFpTest, ChromaGradIsMeasured) {
    // Checkerboard chroma (Cb 110/120, Cr 140/150): every adjacent
    // pair steps by 10 in both channels, so cgrad is exactly 10.0 —
    // and the block still classifies as Skin.
    {
        std::vector<int16_t> y(256, 150), cb(256, 128), cr(256, 128);
        fill_checker_16(cb, 0, 0, 110, 120);
        fill_checker_16(cr, 0, 0, 140, 150);
        auto field = analyze_control_field(y.data(), 16, 16, cb.data(), cr.data());
        ASSERT_EQ(field.size(), 4u);
        EXPECT_EQ(field[0].content, BlockContent::Skin);
        EXPECT_NEAR(field[0].chroma_grad, 10.0f, 0.01f);
    }
    // Smooth sine chroma: still Skin, but cgrad stays below 3.0.
    {
        std::vector<int16_t> y(256, 150), cb(256, 128), cr(256, 128);
        fill_sine_chroma_16(cb, cr);
        auto field = analyze_control_field(y.data(), 16, 16, cb.data(), cr.data());
        ASSERT_EQ(field.size(), 4u);
        EXPECT_EQ(field[0].content, BlockContent::Skin);
        EXPECT_LT(field[0].chroma_grad, 3.0f);
        EXPECT_GT(field[0].chroma_grad, 0.0f);
    }
}

TEST(SkinFpTest, TileDropsMaskedSkinBlock) {
    // 16x16 tile (2x2 blocks): block (0,0) is skin, the rest neutral
    // grey — the same tile as SkinProtectionTest. With the mask
    // dropping block (0,0), no scale entry is emitted; with an
    // all-zero mask the skin scale is emitted (7 B, as before).
    std::vector<int16_t> y(256, 128), cb(256, 128), cr(256, 128);
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) {
            y[r * 16 + c] = 150;
        }
    fill_checker_16(cb, 0, 0, 110, 120);
    fill_checker_16(cr, 0, 0, 140, 150);

    auto encode_with_mask = [&](const std::vector<uint8_t>* mask) {
        TileStats stats;
        auto res = encode_lossy_tile(y.data(), cb.data(), cr.data(),
                                     cb.data(), cr.data(), nullptr,
                                     16, 16, 0, 0, 16, 75.0f, 8, false,
                                     ChromaSubsampling::YUV444, true,
                                     false, false, &stats,
                                     mask == nullptr ? nullptr : mask->data());
        EXPECT_TRUE(res.has_value());
        return stats.v7_scale_bytes;
    };

    const std::vector<uint8_t> drop{1, 0, 0, 0};
    EXPECT_LE(encode_with_mask(&drop), 4u);
    const std::vector<uint8_t> keep{0, 0, 0, 0};
    EXPECT_EQ(encode_with_mask(&keep), 7u);
    EXPECT_EQ(encode_with_mask(nullptr), 7u);
}

TEST(SkinFpTest, WideSkinBandNotTreatedEndToEnd) {
    // 384x64 RGB image: a wide skin band (320x16 px = 40x2 blocks,
    // aspect 20) and a small skin patch (16x16 px = 2x2 blocks), both
    // sine-chroma skin with a 140/160 luma checkerboard, on neutral
    // grey. With the flag on, the patch must reconstruct closer to
    // the original than with the flag off — but the band must
    // reconstruct exactly as with the flag off (dropped by W+C3).
    const uint32_t w = 384, h = 64;
    std::vector<int16_t> y(w * h, 128), cb(w * h, 128), cr(w * h, 128);
    auto paint_skin = [&](uint32_t x0, uint32_t y0, uint32_t pw, uint32_t ph) {
        for (uint32_t r = 0; r < ph; ++r)
            for (uint32_t c = 0; c < pw; ++c) {
                const uint32_t x = x0 + c, yy = y0 + r;
                const size_t i = static_cast<size_t>(yy) * w + x;
                y[i] = ((r + c) % 2 == 0) ? 140 : 160;
                cb[i] = static_cast<int16_t>(
                    std::lround(112.0 + 4.0 * std::sin(2.0 * kPi * x / 16.0)));
                cr[i] = static_cast<int16_t>(
                    std::lround(145.0 + 4.0 * std::cos(2.0 * kPi * yy / 16.0)));
            }
    };
    paint_skin(0, 0, 320, 16);    // wide band: 40x2 blocks
    paint_skin(352, 32, 16, 16);  // small patch: 2x2 blocks

    Image img(w, h, BitDepth::Bits8, false);
    ycbcr_to_rgb(img.pixels().data(), y.data(), cb.data(), cr.data(),
                 w, h, 1, 8, true, false);

    EncoderConfig on;
    on.quality = 75.0f;
    on.subsampling = Subsampling::YUV444;
    on.adaptive_chroma_subsampling = false;
    on.use_control_field = true;
    EncoderConfig off = on;
    off.use_control_field = false;

    auto bytes_on = encode(img, on);
    auto bytes_off = encode(img, off);
    ASSERT_TRUE(bytes_on.has_value());
    ASSERT_TRUE(bytes_off.has_value());

    auto dec_on = decode(std::span<const uint8_t>(*bytes_on));
    auto dec_off = decode(std::span<const uint8_t>(*bytes_off));
    ASSERT_TRUE(dec_on.has_value());
    ASSERT_TRUE(dec_off.has_value());

    std::vector<int16_t> y_on(w * h), cb_on(w * h), cr_on(w * h);
    std::vector<int16_t> y_off(w * h), cb_off(w * h), cr_off(w * h);
    rgb_to_ycbcr(y_on.data(), cb_on.data(), cr_on.data(),
                 dec_on->pixels().data(), w, h, 1, 8, true, false);
    rgb_to_ycbcr(y_off.data(), cb_off.data(), cr_off.data(),
                 dec_off->pixels().data(), w, h, 1, 8, true, false);

    auto region_mse = [&](const std::vector<int16_t>& rec,
                          uint32_t x0, uint32_t y0, uint32_t pw, uint32_t ph) {
        double sse = 0.0;
        for (uint32_t r = 0; r < ph; ++r)
            for (uint32_t c = 0; c < pw; ++c) {
                const size_t i = static_cast<size_t>(y0 + r) * w + (x0 + c);
                const double d = static_cast<double>(rec[i]) - y[i];
                sse += d * d;
            }
        return sse / (static_cast<double>(pw) * ph);
    };
    const double band_on = region_mse(y_on, 0, 0, 320, 16);
    const double band_off = region_mse(y_off, 0, 0, 320, 16);
    EXPECT_DOUBLE_EQ(band_on, band_off);
    const double patch_on = region_mse(y_on, 352, 32, 16, 16);
    const double patch_off = region_mse(y_off, 352, 32, 16, 16);
    EXPECT_LT(patch_on, patch_off);
}
