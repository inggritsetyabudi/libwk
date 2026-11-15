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
#include <vector>

#include "../src/adaptive_chroma.h"

using namespace wk;

namespace {

// Builds a W x H chroma plane filled with `value`.
std::vector<int16_t> flat_plane(uint32_t w, uint32_t h, int16_t value) {
    return std::vector<int16_t>(static_cast<size_t>(w) * h, value);
}

}  // namespace

TEST(AdaptiveChroma, FlatChromaHasZeroLaplacian) {
    auto cb = flat_plane(16, 16, 128);
    auto cr = flat_plane(16, 16, 128);
    EXPECT_DOUBLE_EQ(chroma_laplacian(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16), 0.0);
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16));
}

TEST(AdaptiveChroma, LinearRampHasZeroLaplacian) {
    // A linear ramp has zero second derivative, so the Laplacian is 0.
    std::vector<int16_t> cb(16 * 16), cr(16 * 16, 128);
    for (uint32_t r = 0; r < 16; ++r) {
        for (uint32_t c = 0; c < 16; ++c) {
            cb[r * 16 + c] = static_cast<int16_t>(128 + static_cast<int>(c));
        }
    }
    EXPECT_DOUBLE_EQ(chroma_laplacian(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16), 0.0);
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16));
}

TEST(AdaptiveChroma, QuadraticRampGivesExactLaplacian) {
    // Cb = 128 + r^2: the 5-tap Laplacian is exactly -2 at every interior
    // pixel, so c_lap = 0.5 * (2 + 0) = 1.0, below the threshold.
    std::vector<int16_t> cb(16 * 16), cr(16 * 16, 128);
    for (uint32_t r = 0; r < 16; ++r) {
        for (uint32_t c = 0; c < 16; ++c) {
            cb[r * 16 + c] = static_cast<int16_t>(128 + static_cast<int>(r * r));
        }
    }
    EXPECT_DOUBLE_EQ(chroma_laplacian(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16), 1.0);
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16));

    // Cb = 128 + 2*r^2: Laplacian -4, c_lap exactly 2.0, which is NOT
    // below a 2.0 threshold (strict). The default threshold is 2.1,
    // so pass 2.0 explicitly to test the strict boundary.
    for (uint32_t r = 0; r < 16; ++r) {
        for (uint32_t c = 0; c < 16; ++c) {
            cb[r * 16 + c] = static_cast<int16_t>(128 + 2 * static_cast<int>(r * r));
        }
    }
    EXPECT_DOUBLE_EQ(chroma_laplacian(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16), 2.0);
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16, 2.0));
    // And it IS below the default 2.1 threshold.
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16));
}

TEST(AdaptiveChroma, SingleSpikeExceedsThreshold) {
    // 8x8 tile, one interior Cb pixel +100 over flat 128. Laplacian:
    // 400 at the spike, 100 at each of its 4 neighbours, 0 elsewhere,
    // over 36 interior pixels: mean Cb = 800/36, c_lap = 100/9.
    auto cb = flat_plane(8, 8, 128);
    auto cr = flat_plane(8, 8, 128);
    cb[3 * 8 + 3] = 228;
    EXPECT_NEAR(chroma_laplacian(cb.data(), cr.data(), 8, 8, 0, 0, 8, 8), 100.0 / 9.0, 1e-9);
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 8, 8, 0, 0, 8, 8));
    // A higher threshold flips the same tile to 420.
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 8, 8, 0, 0, 8, 8, 12.0));
}

TEST(AdaptiveChroma, RegionOffsetIsRespected) {
    // Spike lives in the (8,8)-(16,16) region of a 32x32 plane.
    auto cb = flat_plane(32, 32, 128);
    auto cr = flat_plane(32, 32, 128);
    cb[(8 + 3) * 32 + (8 + 3)] = 228;
    EXPECT_NEAR(chroma_laplacian(cb.data(), cr.data(), 32, 32, 8, 8, 8, 8), 100.0 / 9.0, 1e-9);
    EXPECT_DOUBLE_EQ(chroma_laplacian(cb.data(), cr.data(), 32, 32, 0, 0, 8, 8), 0.0);
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 32, 32, 8, 8, 8, 8));
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 32, 32, 0, 0, 8, 8));
}

TEST(AdaptiveChroma, DegenerateTileStays444) {
    // No interior pixel (width or height below 3): no evidence the
    // chroma is smooth, so keep YUV444.
    auto cb = flat_plane(8, 8, 128);
    auto cr = flat_plane(8, 8, 128);
    EXPECT_TRUE(std::isinf(chroma_laplacian(cb.data(), cr.data(), 8, 8, 0, 0, 2, 8)));
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 8, 8, 0, 0, 2, 8));
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 8, 8, 0, 0, 8, 1));
}

TEST(AdaptiveChroma, P999CatchesSharpEdgeTheMeanDrowns) {
    // 64x64 tile, one vertical Cb step of 100 at column 32 (the f1
    // step_edge probe, scaled down). The mean is low (only 2 of 62
    // interior columns carry the step) but p999 is high.
    // Mean: 124 pixels at |lap| 100 over 3844 interior Cb pixels,
    // c_lap = 0.5 * 12400/3844 = 1.6129 < 2.1. p999 = 100 (124 of 7688
    // values are 100, more than the 0.1% the percentile needs).
    // sat_p999 = 100 (the col-32 step pixels are saturated, sat 100).
    std::vector<int16_t> cb(64 * 64), cr(64 * 64, 128);
    for (uint32_t r = 0; r < 64; ++r) {
        for (uint32_t c = 0; c < 64; ++c) {
            cb[r * 64 + c] = static_cast<int16_t>(c < 32 ? 128 : 228);
        }
    }
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64);
    EXPECT_NEAR(stats.mean, 1.6129, 1e-3);
    EXPECT_DOUBLE_EQ(stats.p999, 100.0);
    EXPECT_DOUBLE_EQ(stats.sat_p999, 100.0);
    // Second conditions off: the mean alone sends it to 420.
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                            2.1, 0.0, 0.0));
    // p999 alone forces 444, and so does sat_p999 alone.
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             2.1, 79.0, 0.0));
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             2.1, 0.0, 100.0));
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             2.1, 79.0, 100.0));
}

TEST(AdaptiveChroma, SatP999CatchesSmallSaturatedFeature) {
    // 256x256 tile, flat except an 8x8 saturated block (Cr 212, sat 84)
    // at the centre (the d_skin lips, scaled down). Only 60 of 129032
    // |lap| values are non-zero, so the plain p999 is 0 and misses it;
    // but 28 of them sit on saturated pixels, so sat_p999 is 168.
    // Mean c_lap = 5376 / 129032 = 0.0417 < 2.1.
    std::vector<int16_t> cb(256 * 256, 128), cr(256 * 256, 128);
    for (uint32_t r = 124; r < 132; ++r) {
        for (uint32_t c = 124; c < 132; ++c) {
            cr[r * 256 + c] = 212;
        }
    }
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 256, 256, 0, 0, 256, 256);
    EXPECT_NEAR(stats.mean, 0.0417, 1e-3);
    EXPECT_DOUBLE_EQ(stats.p999, 0.0);
    EXPECT_DOUBLE_EQ(stats.sat_p999, 168.0);
    // Mean alone, and p999 alone, both send it to 420 (the d_skin miss).
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 256, 256, 0, 0, 256, 256,
                                            2.1, 0.0, 0.0));
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 256, 256, 0, 0, 256, 256,
                                            2.1, 79.0, 0.0));
    // The saturated p999 catches it and forces 444.
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 256, 256, 0, 0, 256, 256,
                                             2.1, 0.0, 100.0));
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 256, 256, 0, 0, 256, 256,
                                             2.1, 79.0, 100.0));
}
