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
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace wk {
namespace {

// Histogram buckets for the |Laplacian| percentiles. Values are clamped
// into the top bucket. Exact for 8-bit (|lap| <= 1020) and 10-bit
// (|lap| <= 4092); for 12-bit, values above 8191 clamp, which only
// distorts a percentile that itself sits above 8191 (not realistic:
// that would need 0.1% of pixels to carry a chroma step over 2000).
constexpr int kHistBuckets = 8192;

// Smallest value v whose cumulative count reaches the q-quantile
// (nearest-rank; within 1 of the linearly interpolated percentile for
// integer data).
double percentile_from_hist(const std::vector<size_t>& hist, size_t total, double q) {
    if (total == 0) {
        return -1.0;
    }
    const size_t need = static_cast<size_t>(std::ceil(q * static_cast<double>(total)));
    size_t acc = 0;
    for (int v = 0; v < kHistBuckets; ++v) {
        acc += hist[static_cast<size_t>(v)];
        if (acc >= need) {
            return static_cast<double>(v);
        }
    }
    return static_cast<double>(kHistBuckets - 1);
}

// DCT-II cosine table for the 1..3 band (u, v in 1..3):
// cos(pi*u*(2x+1)/16). Same table as dct.cpp, double precision, only
// the rows the band needs.
const std::array<std::array<double, 8>, 3> kDctBandCos = [] {
    constexpr double kPi = 3.14159265358979323846;
    std::array<std::array<double, 8>, 3> table{};
    for (int u = 0; u < 3; ++u) {
        for (int x = 0; x < 8; ++x) {
            table[static_cast<size_t>(u)][static_cast<size_t>(x)] =
                std::cos(kPi * static_cast<double>((2 * x + 1) * (u + 1)) / 16.0);
        }
    }
    return table;
}();

// Fraction of the region's detrended chroma AC energy in the DCT band
// (u, v) in 1..3, pooled over Cb+Cr and over all 8x8 blocks. The region
// is cropped to a multiple of 8 in each dimension (same as the Python
// replica); each block is detrended by its own mean, so a linear ramp
// contributes ~0. The denominator is the sum of squares of the
// detrended pixels (Parseval: equal to the sum of all squared DCT
// coefficients), so only the 9 band coefficients need computing.
// Returns 0 when the region holds no 8x8 block or no AC energy.
double chroma_dct_band(const int16_t* cb_plane, const int16_t* cr_plane,
                       uint32_t plane_width, uint32_t rx0, uint32_t ry0,
                       uint32_t rx1, uint32_t ry1) {
    const uint32_t w8 = (rx1 - rx0) / 8;
    const uint32_t h8 = (ry1 - ry0) / 8;
    if (w8 == 0 || h8 == 0) {
        return 0.0;
    }
    double e_band = 0.0;
    double e_ac = 0.0;
    const int16_t* planes[2] = {cb_plane, cr_plane};
    for (const int16_t* plane : planes) {
        for (uint32_t by = 0; by < h8; ++by) {
            for (uint32_t bx = 0; bx < w8; ++bx) {
                double block[8][8];
                double mean = 0.0;
                for (uint32_t y = 0; y < 8; ++y) {
                    for (uint32_t x = 0; x < 8; ++x) {
                        const size_t idx =
                            static_cast<size_t>(ry0 + by * 8 + y) * plane_width +
                            (rx0 + bx * 8 + x);
                        block[y][x] = static_cast<double>(plane[idx]);
                        mean += block[y][x];
                    }
                }
                mean /= 64.0;
                // Row pass: R[u][y] = sum_x d(x,y) * cos_u[x].
                double row_sum[3][8] = {};
                for (uint32_t y = 0; y < 8; ++y) {
                    for (uint32_t x = 0; x < 8; ++x) {
                        block[y][x] -= mean;
                        e_ac += block[y][x] * block[y][x];
                        for (int u = 0; u < 3; ++u) {
                            row_sum[u][y] +=
                                block[y][x] * kDctBandCos[static_cast<size_t>(u)][x];
                        }
                    }
                }
                // Column pass: D(u,v) = 0.25 * sum_y R[u][y] * cos_v[y]
                // (orthonormal scale: 0.5 * 0.5 for u, v >= 1).
                for (int u = 0; u < 3; ++u) {
                    for (int v = 0; v < 3; ++v) {
                        double d = 0.0;
                        for (uint32_t y = 0; y < 8; ++y) {
                            d += row_sum[u][y] * kDctBandCos[static_cast<size_t>(v)][y];
                        }
                        d *= 0.25;
                        e_band += d * d;
                    }
                }
            }
        }
    }
    return e_ac > 0.0 ? e_band / e_ac : 0.0;
}

}  // namespace

ChromaLaplacianStats chroma_laplacian_stats(const int16_t* cb_plane, const int16_t* cr_plane,
                                           uint32_t plane_width, uint32_t plane_height,
                                           uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
                                           uint8_t bit_depth) {
    ChromaLaplacianStats out;
    if (cb_plane == nullptr || cr_plane == nullptr || plane_width == 0 || plane_height == 0) {
        out.mean = std::numeric_limits<double>::infinity();
        out.p999 = std::numeric_limits<double>::infinity();
        return out;
    }
    // Clamp the region to the plane; the 5-tap stencil needs all four
    // neighbours inside the region, so only interior pixels count.
    const uint32_t rx0 = std::min(x0, plane_width);
    const uint32_t ry0 = std::min(y0, plane_height);
    const uint32_t rx1 = std::min(x0 + tw, plane_width);
    const uint32_t ry1 = std::min(y0 + th, plane_height);
    if (rx1 - rx0 < 3 || ry1 - ry0 < 3) {
        out.mean = std::numeric_limits<double>::infinity();
        out.p999 = std::numeric_limits<double>::infinity();
        return out;
    }

    std::vector<size_t> hist(static_cast<size_t>(kHistBuckets), 0);
    std::vector<size_t> hist_sat(static_cast<size_t>(kHistBuckets), 0);
    // The saturation threshold and mid scale with bit depth (40/128 at
    // 8-bit, 160/512 at 10-bit, 640/2048 at 12-bit); below 8-bit the
    // 8-bit values apply.
    const int bd_shift = std::max(0, static_cast<int>(bit_depth) - 8);
    const int mid = kAdaptiveChromaMid << bd_shift;
    const int sat_threshold = kAdaptiveChromaSaturationThreshold << bd_shift;
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
            const int a_cb = std::abs(cb_lap);
            const int a_cr = std::abs(cr_lap);
            sum += static_cast<double>(a_cb + a_cr);
            ++count;
            hist[static_cast<size_t>(std::min(a_cb, kHistBuckets - 1))]++;
            hist[static_cast<size_t>(std::min(a_cr, kHistBuckets - 1))]++;
            const int sat = std::max(std::abs(static_cast<int>(cb_plane[idx]) - mid),
                                     std::abs(static_cast<int>(cr_plane[idx]) - mid));
            if (sat > sat_threshold) {
                hist_sat[static_cast<size_t>(std::min(a_cb, kHistBuckets - 1))]++;
                hist_sat[static_cast<size_t>(std::min(a_cr, kHistBuckets - 1))]++;
                out.sat_count += 2;
            }
        }
    }
    if (count == 0) {
        out.mean = std::numeric_limits<double>::infinity();
        out.p999 = std::numeric_limits<double>::infinity();
        return out;
    }
    // c_lap = 0.5 * (mean|lap Cb| + mean|lap Cr|) = sum / (2 * count).
    out.mean = sum / (2.0 * static_cast<double>(count));
    out.p999 = percentile_from_hist(hist, 2 * count, 0.999);
    if (out.sat_count >= kAdaptiveChromaMinSaturatedSamples) {
        out.sat_p999 = percentile_from_hist(hist_sat, out.sat_count, 0.999);
    }
    // frac_gt2: fraction of |Laplacian| values above the cut, from the
    // same histogram. The cut scales with bit depth (2 at 8-bit, 8 at
    // 10-bit, 32 at 12-bit), like the thresholds; the fraction itself
    // does not scale.
    const int gt2_cut = 2 << bd_shift;
    size_t above = 0;
    for (int v = gt2_cut + 1; v < kHistBuckets; ++v) {
        above += hist[static_cast<size_t>(v)];
    }
    out.frac_gt2 = static_cast<double>(above) / static_cast<double>(2 * count);
    out.dct_band = chroma_dct_band(cb_plane, cr_plane, plane_width, rx0, ry0, rx1, ry1);
    return out;
}

double chroma_laplacian(const int16_t* cb_plane, const int16_t* cr_plane,
                        uint32_t plane_width, uint32_t plane_height,
                        uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th) {
    return chroma_laplacian_stats(cb_plane, cr_plane, plane_width, plane_height,
                                  x0, y0, tw, th)
        .mean;
}

bool should_subsample_chroma_420(const int16_t* cb_plane, const int16_t* cr_plane,
                                 uint32_t plane_width, uint32_t plane_height,
                                 uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th,
                                 double threshold, double p999_threshold,
                                 double sat_p999_threshold, uint8_t bit_depth,
                                 double frac_gt2_threshold, double dct_band_threshold) {
    const ChromaLaplacianStats stats =
        chroma_laplacian_stats(cb_plane, cr_plane, plane_width, plane_height,
                               x0, y0, tw, th, bit_depth);
    if (stats.mean >= threshold) {
        return false;
    }
    // Second conditions: a sharp edge (plain p999) or a sharp edge in
    // saturated chroma (saturated p999) forces YUV444 even when the
    // mean is low enough for 420.
    if (p999_threshold > 0.0 && stats.p999 >= p999_threshold) {
        return false;
    }
    if (sat_p999_threshold > 0.0 && stats.sat_p999 >= sat_p999_threshold) {
        return false;
    }
    // Third/fourth conditions (#b3a3): dense mid-range |Laplacian|
    // (frac_gt2) or spectrally concentrated chroma (dct_band) forces
    // YUV444 as well — they catch periodic high-frequency chroma the
    // mean and the percentiles are blind to.
    if (frac_gt2_threshold > 0.0 && stats.frac_gt2 >= frac_gt2_threshold) {
        return false;
    }
    if (dct_band_threshold > 0.0 && stats.dct_band >= dct_band_threshold) {
        return false;
    }
    return true;
}

}  // namespace wk
