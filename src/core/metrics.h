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
#pragma once

#include "common.h"
#include <wk/wk.hpp>
#include <array>

namespace wk::metrics {

struct ChannelMetrics {
    double mae = 0.0;
    double mse = 0.0;
    double psnr = 100.0;
    double ssim = 1.0;
};

struct ArtifactMetrics {
    double chroma_mae = 0.0;
    double chroma_mse = 0.0;
    double chroma_psnr = 100.0;
    double weighted_luma_mae = 0.0;
    double weighted_chroma_mae = 0.0;
    double max_abs_error = 0.0;
};

struct ImageStatistics {
    double mean_luma = 0.0;
    double luma_stddev = 0.0;
    double mean_chroma = 0.0;
    double dark_fraction = 0.0;
    double bright_fraction = 0.0;
};

struct ImageMetrics {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t compared_channels = 0;
    bool compared_alpha = false;
    double mae = 0.0;
    double mse = 0.0;
    double psnr = 100.0;
    double ssim = 1.0;
    std::array<ChannelMetrics, 4> channels{};
    std::array<ChannelMetrics, 3> ycbcr{};
    ArtifactMetrics artifacts{};
    ImageStatistics reference_stats{};
    ImageStatistics candidate_stats{};
};

// One sample from a pixel, as a double. An out-of-range pixel or
// channel index returns 0.0 instead of reading past the buffer.
[[nodiscard]] double sample_at(const Image& image, size_t pixel_index, uint8_t channel_index);

Result<ImageMetrics> compare_images(const Image& reference, const Image& candidate, bool include_alpha = true);

}