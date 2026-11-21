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

#include "../src/core/adaptive_chroma.h"

using namespace wk;

namespace {

// Builds a W x H chroma plane filled with `value`.
std::vector<int16_t> flat_plane(uint32_t w, uint32_t h, int16_t value) {
    return std::vector<int16_t>(static_cast<size_t>(w) * h, value);
}

// Builds the b3_a3 synthetic probe (F3 adaptive-chroma experiment):
// chroma gradient + 2D sinusoid of period 8px (= chroma Nyquist),
// amplitude A. Cb = mid + 22*(x/512) + A*sin(2*pi*x/8)*sin(2*pi*y/8),
// Cr = mid - 18*(y/512) + A*cos(2*pi*x/8)*cos(2*pi*y/8), with mid and
// amplitude scaled by `scale` (1 = 8-bit, 4 = 10-bit, 16 = 12-bit).
// The gradient denominators are the full 512px probe size, not the
// tile size: the tile is a crop of the probe, and a gradient over the
// tile width would be 8x too steep (its rounding staircase would
// pollute the Laplacian and dilute dct_band).
void b3_probe(uint32_t w, uint32_t h, double amplitude, int scale,
              std::vector<int16_t>& cb, std::vector<int16_t>& cr) {
    constexpr double kPi = 3.14159265358979323846;
    const double mid = 128.0 * scale;
    cb.assign(static_cast<size_t>(w) * h, 0);
    cr.assign(static_cast<size_t>(w) * h, 0);
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t c = 0; c < w; ++c) {
            const double sx = std::sin(2.0 * kPi * static_cast<double>(c) / 8.0);
            const double sy = std::sin(2.0 * kPi * static_cast<double>(r) / 8.0);
            const double cx = std::cos(2.0 * kPi * static_cast<double>(c) / 8.0);
            const double cy = std::cos(2.0 * kPi * static_cast<double>(r) / 8.0);
            const double cb_v = mid + 22.0 * scale * static_cast<double>(c) / 512.0 +
                                amplitude * scale * sx * sy;
            const double cr_v = mid - 18.0 * scale * static_cast<double>(r) / 512.0 +
                                amplitude * scale * cx * cy;
            cb[static_cast<size_t>(r) * w + c] = static_cast<int16_t>(std::lround(cb_v));
            cr[static_cast<size_t>(r) * w + c] = static_cast<int16_t>(std::lround(cr_v));
        }
    }
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

// ---- b3_a3 periodic-HF failure mode (hole #9) ----

TEST(AdaptiveChroma, B3TileA3Stats) {
    // 64x64 b3 probe, amplitude 3.0 (the b3_a3.0 case): the 5-tap
    // Laplacian barely sees the 8px sinusoid (mean 1.58 < 2.1, p999 5),
    // but the spectrum is concentrated in one band (dct_band ~0.96)
    // and a third of the |Laplacian| values exceed 2 (frac_gt2 ~0.30).
    // Python replica of this exact synthetic pattern (analyze2.py):
    // mean 1.582, p999 5, frac_gt2 0.303, dct_band 0.956. (The full
    // probe's float values are 1.712 / 8 / 0.300 / 0.947 — the gaps are
    // the RGB round-trip noise the synthetic tile lacks.)
    std::vector<int16_t> cb, cr;
    b3_probe(64, 64, 3.0, 1, cb, cr);
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64);
    EXPECT_NEAR(stats.mean, 1.58, 0.15);
    EXPECT_DOUBLE_EQ(stats.p999, 5.0);
    EXPECT_NEAR(stats.frac_gt2, 0.30, 0.03);
    EXPECT_NEAR(stats.dct_band, 0.96, 0.04);
    // Without the new conditions the tile goes 420 (the bug).
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                            2.1, 79.0, 100.0));
    // With them it goes 444.
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             2.1, 79.0, 100.0, 8, 0.28, 0.50));
}

TEST(AdaptiveChroma, B3TileA2CaughtByDctBandOnly) {
    // Amplitude 2.0: frac_gt2 0.147 is BELOW 0.28, so the third
    // condition alone does not catch it; dct_band 0.915 does.
    // Python replica of this exact synthetic pattern: mean 1.008,
    // frac_gt2 0.147, dct_band 0.915.
    std::vector<int16_t> cb, cr;
    b3_probe(64, 64, 2.0, 1, cb, cr);
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64);
    EXPECT_NEAR(stats.mean, 1.01, 0.15);
    EXPECT_NEAR(stats.frac_gt2, 0.15, 0.03);
    EXPECT_NEAR(stats.dct_band, 0.92, 0.04);
    // frac_gt2 alone: still 420 (tolerated at -2.72 dB).
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                            2.1, 79.0, 100.0, 8, 0.28, 0.0));
    // dct_band alone: 444.
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             2.1, 79.0, 100.0, 8, 0.0, 0.50));
    // Both on: 444.
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             2.1, 79.0, 100.0, 8, 0.28, 0.50));
}

TEST(AdaptiveChroma, SafeGradientTileStays420) {
    // Gradient only (amplitude 0, the a_sky shape): a linear ramp has
    // ~zero Laplacian and its DCT energy sits on the (u,0)/(0,v) axes,
    // outside the 1..3 band, so dct_band is ~0 and the tile stays 420.
    // Python replica: mean 0.081 (the rounding staircase), dct_band 0.
    std::vector<int16_t> cb, cr;
    b3_probe(64, 64, 0.0, 1, cb, cr);
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64);
    EXPECT_NEAR(stats.mean, 0.08, 0.05);
    EXPECT_DOUBLE_EQ(stats.frac_gt2, 0.0);
    EXPECT_NEAR(stats.dct_band, 0.0, 0.02);
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                            2.1, 79.0, 100.0, 8, 0.28, 0.50));
}

TEST(AdaptiveChroma, FlatTileDctBandIsZero) {
    // No AC energy at all: dct_band is 0 by the e_ac == 0 guard.
    auto cb = flat_plane(16, 16, 128);
    auto cr = flat_plane(16, 16, 128);
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16);
    EXPECT_DOUBLE_EQ(stats.dct_band, 0.0);
    EXPECT_DOUBLE_EQ(stats.frac_gt2, 0.0);
    EXPECT_TRUE(should_subsample_chroma_420(cb.data(), cr.data(), 16, 16, 0, 0, 16, 16,
                                            2.1, 79.0, 100.0, 8, 0.28, 0.50));
}

TEST(AdaptiveChroma, B3Tile10BitSpotCheck) {
    // Same A=3.0 pattern scaled x4 (mid 512): dct_band is a fraction
    // so it does not scale, and the frac_gt2 cut scales 2 << (bd-8) =
    // 8. Thresholds scale with the code-value range (x4), as at the
    // encoder call site. Expect the same 444 decision as 8-bit.
    // Python replica: mean 5.354, frac_gt2 (cut 8) 0.248, dct_band
    // 0.985. Note frac_gt2 0.248 is BELOW 0.28 here, so dct_band
    // carries the decision at 10-bit; the separation still holds with
    // a wide margin (0.985 vs the 0.50 threshold).
    std::vector<int16_t> cb, cr;
    b3_probe(64, 64, 3.0, 4, cb, cr);
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64, 10);
    EXPECT_NEAR(stats.mean, 5.35, 0.6);
    EXPECT_NEAR(stats.frac_gt2, 0.25, 0.03);
    EXPECT_NEAR(stats.dct_band, 0.99, 0.04);
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             8.4, 316.0, 400.0, 10, 0.28, 0.50));
    // Gradient-only at 10-bit stays 420.
    std::vector<int16_t> gcb, gcr;
    b3_probe(64, 64, 0.0, 4, gcb, gcr);
    EXPECT_TRUE(should_subsample_chroma_420(gcb.data(), gcr.data(), 64, 64, 0, 0, 64, 64,
                                            8.4, 316.0, 400.0, 10, 0.28, 0.50));
}

TEST(AdaptiveChroma, B3Tile12BitSpotCheck) {
    // Same A=3.0 pattern scaled x16 (mid 2048); thresholds x16.
    // Python replica: mean 20.864, frac_gt2 (cut 32) 0.313, dct_band
    // 0.987 — the separation holds at 12-bit too.
    std::vector<int16_t> cb, cr;
    b3_probe(64, 64, 3.0, 16, cb, cr);
    const auto stats = chroma_laplacian_stats(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64, 12);
    EXPECT_NEAR(stats.mean, 20.86, 2.5);
    EXPECT_NEAR(stats.frac_gt2, 0.31, 0.03);
    EXPECT_NEAR(stats.dct_band, 0.99, 0.04);
    EXPECT_FALSE(should_subsample_chroma_420(cb.data(), cr.data(), 64, 64, 0, 0, 64, 64,
                                             33.6, 1264.0, 1600.0, 12, 0.28, 0.50));
    // Gradient-only at 12-bit stays 420.
    std::vector<int16_t> gcb, gcr;
    b3_probe(64, 64, 0.0, 16, gcb, gcr);
    EXPECT_TRUE(should_subsample_chroma_420(gcb.data(), gcr.data(), 64, 64, 0, 0, 64, 64,
                                            33.6, 1264.0, 1600.0, 12, 0.28, 0.50));
}
