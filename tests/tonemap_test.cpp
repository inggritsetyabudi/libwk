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

// Slice 3 tone-mapping tests (2026-11-19). The operator was chosen by
// measurement (tools/tonemap_slice3_2026-11-19.md): Reinhard with a
// geometric-mean key value. These tests pin the operator's properties,
// not its measured image scores (those live in the harness).
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../tools/tonemap.h"

namespace {

using namespace wkview;

// Dark-scene key scale (memorial: geo 0.000281 -> 0.18/geo = 640) and
// bright-scene key scale (vinesunset: geo 0.0300 -> 6.0), from the
// measurement harness. A peak-white pixel must land near white in a
// dark scene and keep headroom in a bright scene.
constexpr double kDarkSceneKeyScale = 640.0;
constexpr double kBrightSceneKeyScale = 6.0;

TEST(TonemapChannel, BlackMapsToBlack) {
    EXPECT_EQ(tonemap_channel(0.0, kDarkSceneKeyScale), 0);
    EXPECT_EQ(tonemap_channel(0.0, kBrightSceneKeyScale), 0);
    EXPECT_EQ(tonemap_channel(0.0, 1.0), 0);
}

TEST(TonemapChannel, MonotonicNoNanNoInf) {
    double previous = 0.0;
    for (int i = 0; i <= 100; ++i) {
        const double linear = static_cast<double>(i) / 100.0;
        const double out = static_cast<double>(tonemap_channel(linear, 50.0));
        EXPECT_FALSE(std::isnan(out)) << "i=" << i;
        EXPECT_FALSE(std::isinf(out)) << "i=" << i;
        EXPECT_GE(out, previous) << "i=" << i;
        EXPECT_LE(out, 255.0) << "i=" << i;
        previous = out;
    }
}

TEST(TonemapChannel, PeakWhiteNearWhiteInDarkSceneKeepsHeadroomInBrightScene) {
    EXPECT_GE(tonemap_channel(1.0, kDarkSceneKeyScale), 250);
    const uint8_t bright = tonemap_channel(1.0, kBrightSceneKeyScale);
    EXPECT_GT(bright, 200);
    EXPECT_LT(bright, 255);
}

TEST(TonemapChannel, NegativeAndOvershootClamp) {
    EXPECT_EQ(tonemap_channel(-1.0, 50.0), 0);
    EXPECT_EQ(tonemap_channel(1e6, 50.0), 255);
    EXPECT_FALSE(std::isnan(static_cast<double>(tonemap_channel(0.5, 0.0))));
}

TEST(TonemapKeyScale, GeometricMeanMapsToKeyValue) {
    // geo 0.001 -> 0.18 / 0.001 = 180.
    EXPECT_NEAR(tonemap_key_scale(0.001), 180.0, 1e-9);
    // Uniform luminance: geometric mean is the value itself (+1e-6 eps).
    const std::vector<double> lum(64, 0.01);
    const double geo = tonemap_geometric_mean(lum.data(), lum.size());
    EXPECT_NEAR(geo, 0.010001, 1e-6);
    EXPECT_NEAR(tonemap_key_scale(geo), 0.18 / 0.010001, 1e-6);
    // Empty input: no pixels, no crash, finite scale.
    EXPECT_TRUE(std::isfinite(tonemap_key_scale(tonemap_geometric_mean(nullptr, 0))));
}

TEST(TonemapSample, PqBlackToBlackWhiteNearWhiteMonotonic) {
    constexpr uint16_t kMax10 = 1023;
    EXPECT_EQ(tonemap_to_u8(0, kMax10, 16, kDarkSceneKeyScale), 0);
    EXPECT_GE(tonemap_to_u8(kMax10, kMax10, 16, kDarkSceneKeyScale), 250);
    uint8_t previous = 0;
    for (uint16_t code = 0; code <= kMax10; code += 64) {
        const uint8_t out = tonemap_to_u8(code, kMax10, 16, kDarkSceneKeyScale);
        EXPECT_GE(out, previous) << "code=" << code;
        previous = out;
    }
    // HLG white is also near white with a dark-scene key; black is black.
    EXPECT_EQ(tonemap_to_u8(0, kMax10, 18, kDarkSceneKeyScale), 0);
    EXPECT_GE(tonemap_to_u8(kMax10, kMax10, 18, kDarkSceneKeyScale), 250);
}

TEST(TonemapSample, SdrTransferKeepsLinearDownscale) {
    // Transfer 1/13 (BT.709/sRGB) is display-referred: the viewer keeps
    // the historical linear downscale, tone-mapping never applies.
    // 10-bit mid code 512 -> (512*255 + 511)/1023 = 128.
    EXPECT_EQ(tonemap_to_u8(512, 1023, 1, kDarkSceneKeyScale), 128);
    EXPECT_EQ(tonemap_to_u8(512, 1023, 13, kDarkSceneKeyScale), 128);
    EXPECT_EQ(tonemap_to_u8(0, 1023, 1, kDarkSceneKeyScale), 0);
    EXPECT_EQ(tonemap_to_u8(1023, 1023, 1, kDarkSceneKeyScale), 255);
    EXPECT_EQ(tonemap_to_u8(2048, 4095, 1, kDarkSceneKeyScale), 128);
}

TEST(TonemapSample, PqMidtoneBrighterThanLinear) {
    // The measured fix for memorial: a PQ mid code must come out
    // brighter than the linear downscale (code/1023*255) with the
    // dark-scene key, not darker.
    constexpr uint16_t kMax10 = 1023;
    const uint16_t code = 512;
    const uint8_t linear = static_cast<uint8_t>((static_cast<uint32_t>(code) * 255u + 511u) / 1023u);
    EXPECT_GT(tonemap_to_u8(code, kMax10, 16, kDarkSceneKeyScale), linear);
}

TEST(GamutMap, Bt2020MatrixAndClamp) {
    // Pure primaries: matrix rows, negatives clamped to 0, values
    // above 1 left for the Reinhard tone-map (not clamped to 1).
    const std::array<double, 3> r = gamut_map_bt2020_to_srgb(1.0, 0.0, 0.0);
    EXPECT_NEAR(r[0], 1.6604910, 1e-6);
    EXPECT_EQ(r[1], 0.0);
    EXPECT_EQ(r[2], 0.0);
    const std::array<double, 3> g = gamut_map_bt2020_to_srgb(0.0, 1.0, 0.0);
    EXPECT_EQ(g[0], 0.0);
    EXPECT_NEAR(g[1], 1.1328999, 1e-6);
    EXPECT_EQ(g[2], 0.0);
    const std::array<double, 3> b = gamut_map_bt2020_to_srgb(0.0, 0.0, 1.0);
    EXPECT_EQ(b[0], 0.0);
    EXPECT_EQ(b[1], 0.0);
    EXPECT_NEAR(b[2], 1.1187297, 1e-6);
    // White is preserved (matrix rows sum to 1).
    const std::array<double, 3> w = gamut_map_bt2020_to_srgb(1.0, 1.0, 1.0);
    EXPECT_NEAR(w[0], 1.0, 1e-6);
    EXPECT_NEAR(w[1], 1.0, 1e-6);
    EXPECT_NEAR(w[2], 1.0, 1e-6);
    // In-gamut mix: matrix applied exactly, nothing clamped.
    const std::array<double, 3> m = gamut_map_bt2020_to_srgb(0.5, 0.25, 0.1);
    EXPECT_NEAR(m[0], 1.6604910 * 0.5 - 0.5876411 * 0.25 - 0.0728499 * 0.1, 1e-9);
    EXPECT_NEAR(m[1], -0.1245505 * 0.5 + 1.1328999 * 0.25 - 0.0083494 * 0.1, 1e-9);
    EXPECT_NEAR(m[2], -0.0181508 * 0.5 - 0.1005789 * 0.25 + 1.1187297 * 0.1, 1e-9);
}

TEST(GamutMap, NeedsMappingOnlyForBt2020) {
    EXPECT_TRUE(needs_gamut_mapping(9));
    EXPECT_FALSE(needs_gamut_mapping(1));
    EXPECT_FALSE(needs_gamut_mapping(0));
    EXPECT_FALSE(needs_gamut_mapping(2));
    EXPECT_FALSE(needs_gamut_mapping(22));
}

TEST(TonemapPixel, Bt2020PrimariesStayPureAndDominant) {
    // Chart key scale for the 400-nit Band B patches, measured in
    // holes/gamut/gamut_measure.py: clamp -> (222,0,0), (0,211,0),
    // (0,0,211); white patch (1000 nits) -> (232,232,232). Ranges
    // allow float rounding, but the zero channels must stay zero:
    // that is the fix (no gamut step leaked the primaries into the
    // other sRGB channels).
    constexpr double kChartKeyScale = 41.3151;
    uint8_t r = 0, g = 0, b = 0;
    tonemap_pixel_rgb(0.04, 0.0, 0.0, 9, kChartKeyScale, r, g, b);
    EXPECT_GE(r, 218);
    EXPECT_LE(r, 226);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
    tonemap_pixel_rgb(0.0, 0.04, 0.0, 9, kChartKeyScale, r, g, b);
    EXPECT_EQ(r, 0);
    EXPECT_GE(g, 209);
    EXPECT_LE(g, 215);
    EXPECT_EQ(b, 0);
    tonemap_pixel_rgb(0.0, 0.0, 0.04, 9, kChartKeyScale, r, g, b);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 0);
    EXPECT_GE(b, 209);
    EXPECT_LE(b, 215);
    // White patch stays neutral.
    tonemap_pixel_rgb(0.1, 0.1, 0.1, 9, kChartKeyScale, r, g, b);
    EXPECT_EQ(r, g);
    EXPECT_EQ(g, b);
    EXPECT_GE(r, 228);
    EXPECT_LE(r, 236);
    // BT.709 primaries: no mapping, identical to the per-channel path.
    uint8_t r2 = 0, g2 = 0, b2 = 0;
    tonemap_pixel_rgb(0.04, 0.0, 0.0, 1, kChartKeyScale, r2, g2, b2);
    EXPECT_EQ(r2, tonemap_channel(0.04, kChartKeyScale));
    EXPECT_EQ(g2, 0);
    EXPECT_EQ(b2, 0);
}

}  // namespace
