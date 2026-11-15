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

#include <cstddef>
#include <cstdint>

namespace wk {

// Threshold for the adaptive chroma subsampling decision (SPEC v0.2.6):
// a tile whose c_lap is below this is encoded YUV420. 2.1, not 2.0:
// the encoder measures c_lap on its int16 chroma planes, and integer
// rounding adds a small positive bias (~+0.05 avg, up to +0.13) versus
// the float reference the 2.0 value was validated on, so 2.1 reproduces
// the same tile selection. Still far inside the robust range (the
// dangerous tile sits at 6.9).
inline constexpr double kAdaptiveChromaLaplacianThreshold = 2.1;

// Saturation threshold for the second condition, in 8-bit chroma units:
// a pixel counts as "saturated" when max(|Cb-128|, |Cr-128|) exceeds
// this. The threshold and mid scale with bit depth (see
// chroma_laplacian_stats): 40/128 at 8-bit, 160/512 at 10-bit,
// 640/2048 at 12-bit.
inline constexpr int kAdaptiveChromaSaturationThreshold = 40;
// Mid-point of the 8-bit chroma range (see above).
inline constexpr int kAdaptiveChromaMid = 128;
// Minimum number of saturated |Laplacian| samples for sat_p999 to be
// meaningful; below this the tile has too few saturated pixels to judge.
inline constexpr size_t kAdaptiveChromaMinSaturatedSamples = 10;

// Per-tile Laplacian statistics for the adaptive chroma decision:
//   mean: c_lap, the mean |Laplacian| (the V11 criterion).
//   p999: 99.9th percentile of the combined Cb+Cr |Laplacian| values.
//   sat_p999: 99.9th percentile of |Laplacian| at pixels whose saturation
//     exceeds kAdaptiveChromaSaturationThreshold; -1 when there are fewer
//     than kAdaptiveChromaMinSaturatedSamples such values.
//   sat_count: number of |Laplacian| values at saturated pixels.
// For a degenerate region (no interior pixel) mean and p999 are +inf.
struct ChromaLaplacianStats {
    double mean = 0.0;
    double p999 = 0.0;
    double sat_p999 = -1.0;
    size_t sat_count = 0;
};

// Mean, p999 and saturated p999 of |Laplacian| over a tile region.
// bit_depth scales the saturation threshold and mid (8 by default).
[[nodiscard]] ChromaLaplacianStats chroma_laplacian_stats(
    const int16_t* cb_plane, const int16_t* cr_plane,
    uint32_t plane_width, uint32_t plane_height,
    uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
    uint8_t bit_depth = 8);

// Mean |Laplacian| of the chroma planes over a tile region (SPEC v0.2.6):
// for each interior pixel, |4*C - up - down - left - right| averaged over
// Cb and Cr. Returns +inf when the region has no interior pixel, so the
// caller keeps YUV444 for degenerate tiles.
[[nodiscard]] double chroma_laplacian(const int16_t* cb_plane, const int16_t* cr_plane,
                                      uint32_t plane_width, uint32_t plane_height,
                                      uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th);

// True when the tile's chroma should be subsampled to YUV420.
// Second conditions (0 = disabled): when p999_threshold > 0 and the
// tile's p999 reaches it, or sat_p999_threshold > 0 and the tile's
// sat_p999 reaches it, the tile is forced to YUV444 even when c_lap is
// below the threshold. The plain p999 catches long sharp edges the mean
// drowns (a step edge has p999 79 while its mean is 0.15); the saturated
// p999 catches small saturated features (lips: sat_p999 116 while the
// plain p999 is only 34) that the plain p999 also misses.
[[nodiscard]] bool should_subsample_chroma_420(const int16_t* cb_plane, const int16_t* cr_plane,
                                               uint32_t plane_width, uint32_t plane_height,
                                               uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
                                               double threshold = kAdaptiveChromaLaplacianThreshold,
                                               double p999_threshold = 0.0,
                                               double sat_p999_threshold = 0.0,
                                               uint8_t bit_depth = 8);

}  // namespace wk
