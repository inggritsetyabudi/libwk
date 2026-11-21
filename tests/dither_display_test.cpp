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

#include <cstdint>
#include <random>
#include <vector>

#include "../tools/dither_display.h"

// Display dither (wkview --dither): Gaussian sigma=1, seed 12345,
// Flat/Sky blocks only. Reproduces the measured dither-s1.

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 64;

std::vector<uint8_t> GradientLeftNoiseRight() {
    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3);
    std::mt19937 rng(999);
    std::uniform_int_distribution<int> uniform(0, 255);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const size_t o = (static_cast<size_t>(y) * kW + x) * 3;
            if (x < kW / 2) {
                // Smooth ramp: 100 + x, grey. Flat by construction.
                const uint8_t v = static_cast<uint8_t>(100 + x);
                rgb[o + 0] = v;
                rgb[o + 1] = v;
                rgb[o + 2] = v;
            } else {
                rgb[o + 0] = static_cast<uint8_t>(uniform(rng));
                rgb[o + 1] = static_cast<uint8_t>(uniform(rng));
                rgb[o + 2] = static_cast<uint8_t>(uniform(rng));
            }
        }
    }
    return rgb;
}

std::vector<uint8_t> UniformNoise(uint8_t seed_value) {
    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3);
    std::mt19937 rng(seed_value);
    std::uniform_int_distribution<int> uniform(0, 255);
    for (auto& v : rgb) v = static_cast<uint8_t>(uniform(rng));
    return rgb;
}

}  // namespace

TEST(DitherDisplay, Deterministic) {
    const std::vector<uint8_t> input = GradientLeftNoiseRight();
    std::vector<uint8_t> first = input;
    std::vector<uint8_t> second = input;
    wkview::apply_display_dither(first.data(), kW, kH, 1, true);
    wkview::apply_display_dither(second.data(), kW, kH, 1, true);
    EXPECT_EQ(first, second);
}

TEST(DitherDisplay, MaskedOnly) {
    const std::vector<uint8_t> input = GradientLeftNoiseRight();
    std::vector<uint8_t> out = input;
    wkview::apply_display_dither(out.data(), kW, kH, 1, true);

    // Textured half (x >= 40, clear of the boundary blocks): byte-identical.
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 40; x < kW; ++x) {
            const size_t o = (static_cast<size_t>(y) * kW + x) * 3;
            EXPECT_EQ(out[o + 0], input[o + 0]);
            EXPECT_EQ(out[o + 1], input[o + 1]);
            EXPECT_EQ(out[o + 2], input[o + 2]);
        }
    }

    // Flat half interior (x < 24): many pixels change, all by the same
    // amount on R, G and B (one noise draw per pixel = pure luma noise),
    // and no change exceeds a small bound.
    size_t changed = 0;
    size_t checked = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < 24; ++x) {
            const size_t o = (static_cast<size_t>(y) * kW + x) * 3;
            const int dr = static_cast<int>(out[o + 0]) - static_cast<int>(input[o + 0]);
            const int dg = static_cast<int>(out[o + 1]) - static_cast<int>(input[o + 1]);
            const int db = static_cast<int>(out[o + 2]) - static_cast<int>(input[o + 2]);
            EXPECT_EQ(dr, dg);
            EXPECT_EQ(dg, db);
            EXPECT_LE(std::abs(dr), 6);
            ++checked;
            if (dr != 0) ++changed;
        }
    }
    EXPECT_GT(changed, checked / 3);
}

TEST(DitherDisplay, FullyTexturedUnchanged) {
    const std::vector<uint8_t> input = UniformNoise(7);
    std::vector<uint8_t> out = input;
    wkview::apply_display_dither(out.data(), kW, kH, 1, true);
    EXPECT_EQ(out, input);
}

TEST(DitherDisplay, UniformFlatChanges) {
    std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 3, 128);
    wkview::apply_display_dither(out.data(), kW, kH, 1, true);
    size_t changed = 0;
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
        if (out[i * 3] != 128 || out[i * 3 + 1] != 128 || out[i * 3 + 2] != 128) ++changed;
    }
    EXPECT_GT(changed, static_cast<size_t>(kW) * kH / 2);
}
