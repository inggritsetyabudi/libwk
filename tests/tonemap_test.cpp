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

}  // namespace
