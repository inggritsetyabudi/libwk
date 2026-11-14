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
#include "adaptive_chroma.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wk {

double chroma_laplacian(const int16_t* cb_plane, const int16_t* cr_plane,
                        uint32_t plane_width, uint32_t plane_height,
                        uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th) {
    if (cb_plane == nullptr || cr_plane == nullptr || plane_width == 0 || plane_height == 0) {
        return std::numeric_limits<double>::infinity();
    }
    // Clamp the region to the plane; the 5-tap stencil needs all four
    // neighbours inside the region, so only interior pixels count.
    const uint32_t rx0 = std::min(x0, plane_width);
    const uint32_t ry0 = std::min(y0, plane_height);
    const uint32_t rx1 = std::min(x0 + tw, plane_width);
    const uint32_t ry1 = std::min(y0 + th, plane_height);
    if (rx1 - rx0 < 3 || ry1 - ry0 < 3) {
        return std::numeric_limits<double>::infinity();
    }

    double sum = 0.0;
    size_t count = 0;
    for (uint32_t py = ry0 + 1; py + 1 < ry1; ++py) {
        for (uint32_t px = rx0 + 1; px + 1 < rx1; ++px) {
            const size_t idx = static_cast<size_t>(py) * plane_width + px;
            const int cb_lap = 4 * static_cast<int>(cb_plane[idx]) -
                               static_cast<int>(cb_plane[idx - 1]) -
                               static_cast<int>(cb_plane[idx + 1]) -
                               static_cast<int>(cb_plane[idx - plane_width]) -
                               static_cast<int>(cb_plane[idx + plane_width]);
            const int cr_lap = 4 * static_cast<int>(cr_plane[idx]) -
                               static_cast<int>(cr_plane[idx - 1]) -
                               static_cast<int>(cr_plane[idx + 1]) -
                               static_cast<int>(cr_plane[idx - plane_width]) -
                               static_cast<int>(cr_plane[idx + plane_width]);
            sum += std::abs(cb_lap) + std::abs(cr_lap);
            ++count;
        }
    }
    if (count == 0) {
        return std::numeric_limits<double>::infinity();
    }
    // c_lap = 0.5 * (mean|lap Cb| + mean|lap Cr|) = sum / (2 * count).
    return sum / (2.0 * static_cast<double>(count));
}

bool should_subsample_chroma_420(const int16_t* cb_plane, const int16_t* cr_plane,
                                 uint32_t plane_width, uint32_t plane_height,
                                 uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
                                 double threshold) {
    return chroma_laplacian(cb_plane, cr_plane, plane_width, plane_height,
                            x0, y0, tw, th) < threshold;
}

}  // namespace wk
