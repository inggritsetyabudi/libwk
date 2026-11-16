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

#include "colorspace.h"
#include <algorithm>
#include <cmath>

namespace wk {

void rgb_to_ycbcr(int16_t* y_plane, int16_t* cb_plane, int16_t* cr_plane,
                   const uint8_t* rgb, uint32_t width, uint32_t height,
                   uint8_t cicp_matrix, uint8_t bit_depth, bool full_range,
                   bool has_alpha) {
    const auto mat = get_rgb_to_ycbcr(cicp_matrix);
    const int16_t max_val = static_cast<int16_t>((1 << bit_depth) - 1);
    const double scale = static_cast<double>(max_val);

    double y_offset = 0.0;
    double y_scale = scale;
    double c_offset = scale / 2.0;
    double c_scale = scale;
    if (!full_range) {
        const double y_min = 16.0 * scale / 255.0;
        const double y_max = 235.0 * scale / 255.0;
        const double c_min = 16.0 * scale / 255.0;
        const double c_max = 240.0 * scale / 255.0;
        y_offset = y_min;
        y_scale = y_max - y_min;
        c_offset = (c_min + c_max) / 2.0;
        c_scale = c_max - c_min;
    }

    const bool is_16bit = bit_depth > 8;
    const size_t bytes_per_pixel = is_16bit ? (has_alpha ? 8u : 6u) : (has_alpha ? 4u : 3u);

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            const size_t pixel_index = static_cast<size_t>(row) * width + col;
            double r = 0.0;
            double g = 0.0;
            double b = 0.0;

            if (is_16bit) {
                const uint16_t* pixel = reinterpret_cast<const uint16_t*>(rgb + pixel_index * bytes_per_pixel);
                r = static_cast<double>(pixel[0]) / scale;
                g = static_cast<double>(pixel[1]) / scale;
                b = static_cast<double>(pixel[2]) / scale;
            } else {
                const uint8_t* pixel = rgb + pixel_index * bytes_per_pixel;
                r = static_cast<double>(pixel[0]) / 255.0;
                g = static_cast<double>(pixel[1]) / 255.0;
                b = static_cast<double>(pixel[2]) / 255.0;
            }

            double y_value = 0.0;
            double cb_value = 0.0;
            double cr_value = 0.0;
            mat.transform(r, g, b, y_value, cb_value, cr_value);

            y_plane[pixel_index] = static_cast<int16_t>(std::lround(std::clamp(
                y_value * y_scale + y_offset, 0.0, static_cast<double>(max_val))));
            cb_plane[pixel_index] = static_cast<int16_t>(std::lround(std::clamp(
                cb_value * c_scale + c_offset, 0.0, static_cast<double>(max_val))));
            cr_plane[pixel_index] = static_cast<int16_t>(std::lround(std::clamp(
                cr_value * c_scale + c_offset, 0.0, static_cast<double>(max_val))));
        }
    }
}

void ycbcr_to_rgb(uint8_t* rgb, const int16_t* y_plane,
                   const int16_t* cb_plane, const int16_t* cr_plane,
                   uint32_t width, uint32_t height,
                   uint8_t cicp_matrix, uint8_t bit_depth, bool full_range,
                   bool has_alpha, const int16_t* alpha_plane) {
    const auto mat = get_ycbcr_to_rgb(cicp_matrix);
    const int16_t max_val = static_cast<int16_t>((1 << bit_depth) - 1);
    const double scale = static_cast<double>(max_val);

    double y_offset = 0.0;
    double y_scale = scale;
    double c_offset = scale / 2.0;
    double c_scale = scale;
    if (!full_range) {
        const double y_min = 16.0 * scale / 255.0;
        const double y_max = 235.0 * scale / 255.0;
        const double c_min = 16.0 * scale / 255.0;
        const double c_max = 240.0 * scale / 255.0;
        y_offset = y_min;
        y_scale = y_max - y_min;
        c_offset = (c_min + c_max) / 2.0;
        c_scale = c_max - c_min;
    }

    const bool is_16bit = bit_depth > 8;
    const size_t bytes_per_pixel = is_16bit ? (has_alpha ? 8u : 6u) : (has_alpha ? 4u : 3u);

    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            const size_t pixel_index = static_cast<size_t>(row) * width + col;
            const double y_value = (static_cast<double>(y_plane[pixel_index]) - y_offset) / y_scale;
            const double cb_value = (static_cast<double>(cb_plane[pixel_index]) - c_offset) / c_scale;
            const double cr_value = (static_cast<double>(cr_plane[pixel_index]) - c_offset) / c_scale;

            double r = 0.0;
            double g = 0.0;
            double b = 0.0;
            mat.transform(y_value, cb_value, cr_value, r, g, b);
            r = std::clamp(r, 0.0, 1.0);
            g = std::clamp(g, 0.0, 1.0);
            b = std::clamp(b, 0.0, 1.0);

            if (is_16bit) {
                uint16_t* pixel = reinterpret_cast<uint16_t*>(rgb + pixel_index * bytes_per_pixel);
                pixel[0] = static_cast<uint16_t>(r * scale + 0.5);
                pixel[1] = static_cast<uint16_t>(g * scale + 0.5);
                pixel[2] = static_cast<uint16_t>(b * scale + 0.5);
                if (has_alpha) {
                    pixel[3] = static_cast<uint16_t>(alpha_plane ? alpha_plane[pixel_index] : max_val);
                }
            } else {
                uint8_t* pixel = rgb + pixel_index * bytes_per_pixel;
                pixel[0] = static_cast<uint8_t>(r * 255.0 + 0.5);
                pixel[1] = static_cast<uint8_t>(g * 255.0 + 0.5);
                pixel[2] = static_cast<uint8_t>(b * 255.0 + 0.5);
                if (has_alpha) {
                    pixel[3] = static_cast<uint8_t>(((alpha_plane ? alpha_plane[pixel_index] : max_val) * 255 + max_val / 2) / max_val);
                }
            }
        }
    }
}

void subsample_420(const int16_t* in, int16_t* out,
                    uint32_t width, uint32_t height) {
    const uint32_t out_w = (width + 1) / 2;
    const uint32_t out_h = (height + 1) / 2;

    for (uint32_t row = 0; row < out_h; ++row) {
        for (uint32_t col = 0; col < out_w; ++col) {
            const uint32_t r0 = row * 2;
            const uint32_t r1 = std::min(r0 + 1, height - 1);
            const uint32_t c0 = col * 2;
            const uint32_t c1 = std::min(c0 + 1, width - 1);

            const int sum = in[r0 * width + c0] + in[r0 * width + c1] +
                            in[r1 * width + c0] + in[r1 * width + c1];
            out[row * out_w + col] = static_cast<int16_t>((sum + 2) / 4);
        }
    }
}

void upsample_420(const int16_t* in, int16_t* out,
                   uint32_t width, uint32_t height) {
    // Catmull-Rom bicubic upsampling. Phase matches the 2x2 box-average
    // subsampler above: src = dst*0.5 - 0.25 (the subsampled sample is the
    // centre of its 2x2 block). The result is clamped to the input plane's
    // [min, max] because Catmull-Rom's negative lobes can overshoot.
    const uint32_t in_w = (width + 1) / 2;
    const uint32_t in_h = (height + 1) / 2;

    int16_t lo = in[0];
    int16_t hi = in[0];
    for (size_t i = 1; i < static_cast<size_t>(in_w) * in_h; ++i) {
        lo = std::min(lo, in[i]);
        hi = std::max(hi, in[i]);
    }

    auto sample = [&](int x, int y) -> double {
        x = std::clamp(x, 0, static_cast<int>(in_w) - 1);
        y = std::clamp(y, 0, static_cast<int>(in_h) - 1);
        return static_cast<double>(in[static_cast<size_t>(y) * in_w + static_cast<size_t>(x)]);
    };

    auto catmull_rom = [](double t, double p0, double p1, double p2, double p3) -> double {
        const double t2 = t * t;
        const double t3 = t2 * t;
        return p0 * (-0.5 * t + t2 - 0.5 * t3) + p1 * (1.0 - 2.5 * t2 + 1.5 * t3) +
               p2 * (0.5 * t + 2.0 * t2 - 1.5 * t3) + p3 * (-0.5 * t2 + 0.5 * t3);
    };

    for (uint32_t row = 0; row < height; ++row) {
        const double src_y = static_cast<double>(row) * 0.5 - 0.25;
        const int y0 = static_cast<int>(std::floor(src_y));
        const double ty = src_y - static_cast<double>(y0);

        for (uint32_t col = 0; col < width; ++col) {
            const double src_x = static_cast<double>(col) * 0.5 - 0.25;
            const int x0 = static_cast<int>(std::floor(src_x));
            const double tx = src_x - static_cast<double>(x0);

            double row_vals[4];
            for (int j = -1; j <= 2; ++j) {
                row_vals[j + 1] = catmull_rom(tx, sample(x0 - 1, y0 + j), sample(x0, y0 + j),
                                              sample(x0 + 1, y0 + j), sample(x0 + 2, y0 + j));
            }
            const double v = catmull_rom(ty, row_vals[0], row_vals[1], row_vals[2], row_vals[3]);
            out[static_cast<size_t>(row) * width + col] =
                static_cast<int16_t>(std::lround(std::clamp(v, static_cast<double>(lo),
                                                            static_cast<double>(hi))));
        }
    }
}

}

