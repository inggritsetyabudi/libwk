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
#include <algorithm>
#include <wk/wk.hpp>
#include "../src/metrics.h"

using namespace wk;

namespace {

Image make_rgb_image(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    Image image(width, height, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
    }
    return image;
}

Image make_color_edge_image() {
    Image image(16, 16, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < 16; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 16; ++x) {
            const bool left = x < 8;
            row[x * 3 + 0] = left ? 35 : 215;
            row[x * 3 + 1] = left ? 90 : 60;
            row[x * 3 + 2] = left ? 210 : 40;
        }
    }
    return image;
}

}

TEST(MetricsTest, SampleAtReadsInRangeSamples) {
    Image image = make_rgb_image(2, 2, 10, 20, 30);
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 0, 0), 10.0);
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 0, 1), 20.0);
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 3, 2), 30.0);
}

TEST(MetricsTest, SampleAtReturnsZeroOutOfRange) {
    Image image = make_rgb_image(2, 2, 10, 20, 30);
    // Past the last pixel (2x2 has pixels 0..3).
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 4, 0), 0.0);
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 100, 0), 0.0);
    // No alpha channel on this image.
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 0, 3), 0.0);
    EXPECT_DOUBLE_EQ(metrics::sample_at(image, 0, 9), 0.0);
}

TEST(MetricsTest, IdenticalImageIsPerfect) {
    Image reference = make_rgb_image(16, 16, 90, 120, 200);
    auto metrics = wk::metrics::compare_images(reference, reference);
    ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
    EXPECT_DOUBLE_EQ(metrics->mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->mse, 0.0);
    EXPECT_DOUBLE_EQ(metrics->psnr, 100.0);
    EXPECT_NEAR(metrics->ssim, 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(metrics->ycbcr[0].mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->ycbcr[1].mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->ycbcr[2].mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->artifacts.chroma_mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->artifacts.weighted_luma_mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->artifacts.weighted_chroma_mae, 0.0);
    EXPECT_DOUBLE_EQ(metrics->artifacts.max_abs_error, 0.0);
    EXPECT_DOUBLE_EQ(metrics->reference_stats.mean_luma, metrics->candidate_stats.mean_luma);
    EXPECT_DOUBLE_EQ(metrics->reference_stats.mean_chroma, metrics->candidate_stats.mean_chroma);
}

TEST(MetricsTest, AlphaIsIgnoredWhenOnlyOneImageHasIt) {
    Image reference(8, 8, BitDepth::Bits8, true);
    Image candidate(8, 8, BitDepth::Bits8, false);

    for (uint32_t y = 0; y < 8; ++y) {
        uint8_t* ref_row = reference.row(y);
        uint8_t* cand_row = candidate.row(y);
        for (uint32_t x = 0; x < 8; ++x) {
            ref_row[x * 4 + 0] = 10;
            ref_row[x * 4 + 1] = 20;
            ref_row[x * 4 + 2] = 30;
            ref_row[x * 4 + 3] = static_cast<uint8_t>(x * 8);
            cand_row[x * 3 + 0] = 10;
            cand_row[x * 3 + 1] = 20;
            cand_row[x * 3 + 2] = 30;
        }
    }

    auto metrics = wk::metrics::compare_images(reference, candidate);
    ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
    EXPECT_EQ(metrics->compared_channels, 3u);
    EXPECT_FALSE(metrics->compared_alpha);
    EXPECT_DOUBLE_EQ(metrics->mae, 0.0);
}

TEST(MetricsTest, DimensionMismatchRejected) {
    Image reference = make_rgb_image(16, 16, 1, 2, 3);
    Image candidate = make_rgb_image(15, 16, 1, 2, 3);
    auto metrics = wk::metrics::compare_images(reference, candidate);
    ASSERT_FALSE(metrics.has_value());
    EXPECT_EQ(metrics.error().code, ErrorCode::InvalidParameter);
}

TEST(MetricsTest, ColorShiftShowsUpInChromaMetrics) {
    Image reference = make_rgb_image(16, 16, 90, 120, 200);
    Image candidate = make_rgb_image(16, 16, 90, 120, 224);

    auto metrics = wk::metrics::compare_images(reference, candidate);
    ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
    EXPECT_GT(metrics->ycbcr[1].mae, 0.0);
    EXPECT_GT(metrics->ycbcr[2].mae, 0.0);
    EXPECT_GT(metrics->artifacts.chroma_mae, 0.0);
    EXPECT_LT(metrics->artifacts.chroma_psnr, 100.0);
    EXPECT_GE(metrics->artifacts.max_abs_error, 24.0);
}

TEST(MetricsTest, WeightedChromaMetricTracksColorEdges) {
    Image reference = make_color_edge_image();
    Image candidate = reference;

    for (uint32_t y = 0; y < 16; ++y) {
        uint8_t* row = candidate.row(y);
        for (uint32_t x = 7; x <= 8; ++x) {
            row[x * 3 + 0] = static_cast<uint8_t>(std::min(255, row[x * 3 + 0] + 18));
            row[x * 3 + 1] = static_cast<uint8_t>(std::max(0, row[x * 3 + 1] - 12));
            row[x * 3 + 2] = static_cast<uint8_t>(std::max(0, row[x * 3 + 2] - 22));
        }
    }

    auto metrics = wk::metrics::compare_images(reference, candidate);
    ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
    EXPECT_GT(metrics->artifacts.weighted_chroma_mae, metrics->artifacts.chroma_mae);
    EXPECT_GT(metrics->artifacts.weighted_luma_mae, 0.0);
}

TEST(MetricsTest, ImageStatisticsTrackBrightnessBuckets) {
    Image dark = make_rgb_image(16, 16, 18, 18, 18);
    Image bright = make_rgb_image(16, 16, 240, 240, 240);

    auto metrics = wk::metrics::compare_images(dark, bright);
    ASSERT_TRUE(metrics.has_value()) << metrics.error().message;
    EXPECT_LT(metrics->reference_stats.mean_luma, metrics->candidate_stats.mean_luma);
    EXPECT_GT(metrics->reference_stats.dark_fraction, 0.99);
    EXPECT_GT(metrics->candidate_stats.bright_fraction, 0.99);
    EXPECT_LT(metrics->reference_stats.mean_chroma, 1e-6);
    EXPECT_LT(metrics->candidate_stats.mean_chroma, 1e-6);
}