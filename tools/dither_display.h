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

// Display dither for wkview --dither (viewer-only, opt-in, default OFF).
// Reproduces the measured dither-s1 from the deband slice-0 research:
// zero-mean Gaussian noise, sigma = 1 (8-bit units), fixed seed 12345
// (deterministic), applied ONLY to pixels whose 8x8 block classifies
// Flat or Sky via analyze_control_field. The display buffer is RGB, so
// the same noise draw is added to R, G and B: the luma weights sum to
// 1, so luma gets exactly the measured sigma=1 noise while chroma is
// unchanged. Pixels outside Flat/Sky blocks are byte-identical. The
// per-channel fresh seed mirrors the diag's per-plane fresh seed.
// No codec, file, or default behaviour is touched.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "../src/core/colorspace.h"
#include "../src/core/control_field.h"

namespace wkview {

inline void apply_display_dither(uint8_t* rgb, uint32_t width, uint32_t height,
                                 uint8_t cicp_matrix, bool full_range) {
    const size_t npix = static_cast<size_t>(width) * height;
    if (npix == 0 || rgb == nullptr) return;

    // YCbCr planes for classification only; the buffer stays RGB.
    std::vector<int16_t> y_plane(npix), cb_plane(npix), cr_plane(npix);
    wk::rgb_to_ycbcr(y_plane.data(), cb_plane.data(), cr_plane.data(), rgb, width, height,
                     cicp_matrix, 8, full_range, false);
    const std::vector<wk::BlockControl> field =
        wk::analyze_control_field(y_plane.data(), width, height, cb_plane.data(),
                                  cr_plane.data(), 8);

    const uint32_t bpr = (width + 7) / 8;
    std::vector<uint8_t> mask(npix, 0);
    for (uint32_t by = 0; by < (height + 7) / 8; ++by) {
        for (uint32_t bx = 0; bx < bpr; ++bx) {
            const wk::BlockContent content =
                field[static_cast<size_t>(by) * bpr + bx].content;
            if (content != wk::BlockContent::Flat && content != wk::BlockContent::Sky) continue;
            for (uint32_t r = 0; r < 8 && by * 8 + r < height; ++r) {
                for (uint32_t c = 0; c < 8 && bx * 8 + c < width; ++c) {
                    mask[(static_cast<size_t>(by * 8 + r)) * width + (bx * 8 + c)] = 1;
                }
            }
        }
    }

    for (int ch = 0; ch < 3; ++ch) {
        std::mt19937 rng(12345);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (size_t i = 0; i < npix; ++i) {
            if (!mask[i]) continue;
            const size_t o = i * 3 + static_cast<size_t>(ch);
            rgb[o] = static_cast<uint8_t>(
                std::clamp(std::lround(static_cast<double>(rgb[o]) + gauss(rng)), 0L, 255L));
        }
    }
}

}  // namespace wkview
