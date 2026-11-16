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

#include <cmath>
#include <span>
#include <vector>

#include <wk/wk.hpp>

#include "../src/colorspace.h"
#include "../src/control_field.h"
#include "../src/lossy_tile_encode.h"

using namespace wk;

namespace {

// Fills one 8x8 block of a 16x16 plane with a constant value.
void fill_block_16(std::vector<int16_t>& plane, uint32_t bx, uint32_t by, int16_t v) {
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c)
            plane[(by * 8 + r) * 16 + (bx * 8 + c)] = v;
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

// 16x16 tile (2x2 blocks): block (0,0) is skin, the rest neutral grey.
// Skin values are the same as ControlFieldTest.SkinBlockIsSkin: Y 150
// uniform, Cb 110/120, Cr 140/150 (means Cb 115, Cr 145, hue -37 deg,
// chroma std 5 — inside the skin box, past the uniformity gate).
struct TilePlanes {
    std::vector<int16_t> y, cb, cr;
};

TilePlanes make_skin_tile() {
    TilePlanes t;
    t.y.assign(256, 128);
    t.cb.assign(256, 128);
    t.cr.assign(256, 128);
    fill_block_16(t.y, 0, 0, 150);
    fill_checker_16(t.cb, 0, 0, 110, 120);
    fill_checker_16(t.cr, 0, 0, 140, 150);
    return t;
}

Result<TileEncodeResult> encode_tile(const TilePlanes& t, bool use_control_field,
                                     TileStats* stats) {
    return encode_lossy_tile(t.y.data(), t.cb.data(), t.cr.data(),
                             t.cb.data(), t.cr.data(), nullptr,
                             16, 16, 0, 0, 16, 75.0f, 8, false,
                             ChromaSubsampling::YUV444, use_control_field,
                             false, false, stats);
}

}  // namespace

TEST(SkinProtectionTest, SkinBlockEmitsOneLumaScaleEntry) {
    // One Skin block, flag on: the V7 scale stream carries exactly one
    // entry — the luma stream (u16 count + u16 index + u8 scale = 5 B)
    // — and the chroma stream stays empty (u16 count = 2 B). Total 7 B:
    // the skin scale is luma-only, exactly as measured.
    auto tile = make_skin_tile();
    TileStats stats;
    auto res = encode_tile(tile, true, &stats);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(stats.v7_scale_bytes, 7u);
}

TEST(SkinProtectionTest, SkinBlockStaysIdentityWithFlagOff) {
    // Same tile, flag off: no policy emits a non-identity scale, so the
    // scale streams are empty (2 B each in V8+) or absent (V6).
    auto tile = make_skin_tile();
    TileStats stats;
    auto res = encode_tile(tile, false, &stats);
    ASSERT_TRUE(res.has_value());
    EXPECT_LE(stats.v7_scale_bytes, 4u);
}

TEST(SkinProtectionTest, UniformBeigeWallEmitsNoScale) {
    // Perfectly uniform beige (Y=150, Cb=120, Cr=134): inside the skin
    // colour box, but chroma std 0, so the uniformity gate rejects it
    // and no scale entry is emitted even with the flag on.
    TilePlanes t;
    t.y.assign(256, 150);
    t.cb.assign(256, 120);
    t.cr.assign(256, 134);
    TileStats stats;
    auto res = encode_tile(t, true, &stats);
    ASSERT_TRUE(res.has_value());
    EXPECT_LE(stats.v7_scale_bytes, 4u);
}

TEST(SkinProtectionTest, SkinRegionImprovesWithFlagOn) {
    // 32x32 RGB image: one skin block at (0,0) on neutral grey. The
    // skin block's luma is a 140/160 checkerboard (mean 150, still
    // inside the skin box) so the luma residual is real and finer
    // quant buys measurable quality. With the flag on the skin region
    // must reconstruct closer to the original, at the cost of a few
    // more bytes.
    const uint32_t w = 32, h = 32;
    std::vector<int16_t> y(w * h, 128), cb(w * h, 128), cr(w * h, 128);
    // Chroma is a smooth sine, not a checkerboard: real skin is
    // chroma-smooth (cgrad ~0.5 < 3.0), so the block stays treated;
    // a per-pixel checkerboard has cgrad 10 and is dropped by the
    // false-positive rule. Cr std 2.9 still passes the uniformity gate.
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) {
            const bool alt = ((r + c) % 2 == 0);
            y[r * w + c] = alt ? 140 : 160;
            cb[r * w + c] = static_cast<int16_t>(
                std::lround(112.0 + 4.0 * std::sin(2.0 * 3.141592653589793 * c / 16.0)));
            cr[r * w + c] = static_cast<int16_t>(
                std::lround(145.0 + 4.0 * std::cos(2.0 * 3.141592653589793 * r / 16.0)));
        }
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
    EXPECT_GT(bytes_on->size(), bytes_off->size());

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

    // The skin block's luma alternates 140/160; compare each decoded
    // pixel against its own original value.
    double mse_on = 0.0, mse_off = 0.0;
    for (uint32_t r = 0; r < 8; ++r)
        for (uint32_t c = 0; c < 8; ++c) {
            const double expect = ((r + c) % 2 == 0) ? 140.0 : 160.0;
            const double d_on = y_on[r * w + c] - expect;
            const double d_off = y_off[r * w + c] - expect;
            mse_on += d_on * d_on;
            mse_off += d_off * d_off;
        }
    mse_on /= 64.0;
    mse_off /= 64.0;
    EXPECT_LT(mse_on, mse_off);
}
