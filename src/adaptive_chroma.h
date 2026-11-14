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

// Mean |Laplacian| of the chroma planes over a tile region (SPEC v0.2.6):
// for each interior pixel, |4*C - up - down - left - right| averaged over
// Cb and Cr. Returns +inf when the region has no interior pixel, so the
// caller keeps YUV444 for degenerate tiles.
[[nodiscard]] double chroma_laplacian(const int16_t* cb_plane, const int16_t* cr_plane,
                                      uint32_t plane_width, uint32_t plane_height,
                                      uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th);

// True when the tile's chroma should be subsampled to YUV420.
[[nodiscard]] bool should_subsample_chroma_420(const int16_t* cb_plane, const int16_t* cr_plane,
                                               uint32_t plane_width, uint32_t plane_height,
                                               uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
                                               double threshold = kAdaptiveChromaLaplacianThreshold);

}  // namespace wk
