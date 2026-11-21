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

// Slice 3 tone-mapping for wkview (2026-11-19). Viewer-only: the codec
// never calls this, no bitstream or pixel data changes, only the
// 16-bit HDR display path in tools/wkview.cpp.
//
// Operator, chosen by measurement (tools/tonemap_slice3_2026-11-19.md):
// Reinhard with a geometric-mean key value (textbook Reinhard 2002).
// Per image: linear light -> geometric-mean luminance maps to 0.18 ->
// x/(1+x) -> sRGB OETF -> 8-bit. PQ->sRGB without tone-mapping and
// ACES were measured and rejected (crush/clip/banding numbers in the
// report). Reuses wk::apply_eotf / wk::srgb::oetf from
// src/core/colorspace.h; no new EOTF/OETF is written here.
#pragma once

#include "../src/core/colorspace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wkview {

// BT.2020 (CICP primaries 9) is the only wide gamut the viewer remaps:
// its primaries lie outside sRGB, so linear BT.2020 light must pass
// through the 3x3 NPM matrix before the sRGB tone-map. BT.709 (1)
// already shares the sRGB primaries: identity, no mapping.
inline bool needs_gamut_mapping(uint8_t cicp_primaries) {
    return cicp_primaries == 9;
}

// Linear BT.2020 -> linear sRGB (D65). NPM matrix from the ITU-R
// BT.2020 / BT.709 primaries (same matrix as ITU-R BT.2087); rows sum
// to 1, so white is preserved. Out-of-gamut channels clamp at 0:
// measured better than hue-preserving desaturation on the HDR stress
// chart (mean hue error 5.8 deg vs 7.4, saturation kept). Values above
// 1 are left for the Reinhard tone-map.
inline std::array<double, 3> gamut_map_bt2020_to_srgb(double r, double g, double b) {
    return {std::max(1.6604910 * r - 0.5876411 * g - 0.0728499 * b, 0.0),
            std::max(-0.1245505 * r + 1.1328999 * g - 0.0083494 * b, 0.0),
            std::max(-0.0181508 * r - 0.1005789 * g + 1.1187297 * b, 0.0)};
}

// Only PQ (16) and HLG (18) get tone-mapped. SDR transfers (BT.709 1,
// sRGB 13) are display-referred, so the viewer keeps the historical
// linear downscale for them.
inline bool is_hdr_transfer(uint8_t cicp_transfer) {
    return cicp_transfer == 16 || cicp_transfer == 18;
}

// Linear light, 0..1, from one code value. The PQ EOTF returns nits,
// so it is divided by 10000; the HLG inverse OETF already returns
// scene-linear 0..1.
inline double tonemap_linear_rel(uint16_t code, uint16_t max_value, uint8_t cicp_transfer) {
    const double ep = static_cast<double>(code) / static_cast<double>(max_value);
    double linear = wk::apply_eotf(ep, cicp_transfer);
    if (cicp_transfer == 16) {
        linear /= 10000.0;
    }
    return linear;
}

// BT.709 luminance weights, for the key-value pre-pass.
inline double tonemap_luminance(double r, double g, double b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// Geometric mean of per-pixel linear luminance, with the same 1e-6
// floor the measurement harness used (tools/tonemap_measure.py).
// Empty input returns 0.0; tonemap_key_scale floors it, so the scale
// stays finite.
inline double tonemap_geometric_mean(const double* luminance, size_t count) {
    if (count == 0 || luminance == nullptr) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        sum += std::log(luminance[i] + 1e-6);
    }
    return std::exp(sum / static_cast<double>(count));
}

// Textbook Reinhard key value: the geometric mean maps to 0.18.
inline double tonemap_key_scale(double geometric_mean) {
    return 0.18 / std::max(geometric_mean, 1e-6);
}

// Reinhard on linear light with the key value already baked into
// key_scale: x = linear * key_scale, out = x / (1 + x), then sRGB
// OETF. Monotonic, 0 -> 0, finite for any finite input; negative
// linear light clamps to black.
inline uint8_t tonemap_channel(double linear_rel, double key_scale) {
    const double lin = std::max(linear_rel, 0.0);
    const double x = lin * key_scale;
    const double tm = x / (1.0 + x);
    const double srgb = std::clamp(wk::srgb::oetf(tm), 0.0, 1.0);
    return static_cast<uint8_t>(std::floor(srgb * 255.0 + 0.5));
}

// One 16-bit sample to an 8-bit display value. HDR transfers go
// through the tone-map; SDR transfers keep the historical linear
// downscale ((sample*255 + max/2)/max), so SDR pixels never change.
inline uint8_t tonemap_to_u8(uint16_t sample, uint16_t max_value, uint8_t cicp_transfer,
                             double key_scale) {
    if (!is_hdr_transfer(cicp_transfer)) {
        return static_cast<uint8_t>((static_cast<uint32_t>(sample) * 255u + max_value / 2u) /
                                    max_value);
    }
    return tonemap_channel(tonemap_linear_rel(sample, max_value, cicp_transfer), key_scale);
}

// One HDR pixel already in linear light -> 8-bit sRGB, with the
// BT.2020 gamut step when primaries == 9. The key scale comes from
// the frame pre-pass (same value the channel path uses).
inline void tonemap_pixel_rgb(double r_lin, double g_lin, double b_lin,
                              uint8_t cicp_primaries, double key_scale, uint8_t& r_out,
                              uint8_t& g_out, uint8_t& b_out) {
    if (needs_gamut_mapping(cicp_primaries)) {
        const std::array<double, 3> mapped = gamut_map_bt2020_to_srgb(r_lin, g_lin, b_lin);
        r_lin = mapped[0];
        g_lin = mapped[1];
        b_lin = mapped[2];
    }
    r_out = tonemap_channel(r_lin, key_scale);
    g_out = tonemap_channel(g_lin, key_scale);
    b_out = tonemap_channel(b_lin, key_scale);
}

}  // namespace wkview
