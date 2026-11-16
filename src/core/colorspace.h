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
#include <array>
#include <cmath>

namespace wk {

namespace pq {

constexpr double M1 = 0.1593017578125;
constexpr double M2 = 78.84375;
constexpr double C1 = 0.8359375;
constexpr double C2 = 18.8515625;
constexpr double C3 = 18.6875;

inline double eotf(double ep) {
    if (ep <= 0.0) {
        return 0.0;
    }
    const double ep_pow = std::pow(ep, 1.0 / M2);
    const double num = std::max(ep_pow - C1, 0.0);
    const double den = C2 - C3 * ep_pow;
    if (den <= 0.0) {
        return 0.0;
    }
    return 10000.0 * std::pow(num / den, 1.0 / M1);
}

inline double oetf(double y) {
    if (y <= 0.0) {
        return 0.0;
    }
    const double yn = y / 10000.0;
    const double yn_pow = std::pow(yn, M1);
    const double num = C1 + C2 * yn_pow;
    const double den = 1.0 + C3 * yn_pow;
    return std::pow(num / den, M2);
}

}

namespace hlg {

constexpr double A = 0.17883277;
constexpr double B = 0.28466892;
constexpr double C_HLG = 0.55991073;

inline double oetf(double e) {
    if (e <= 0.0) {
        return 0.0;
    }
    if (e <= 1.0 / 12.0) {
        return std::sqrt(3.0 * e);
    }
    return A * std::log(12.0 * e - B) + C_HLG;
}

inline double inv_oetf(double ep) {
    if (ep <= 0.0) {
        return 0.0;
    }
    if (ep <= 0.5) {
        return (ep * ep) / 3.0;
    }
    return (std::exp((ep - C_HLG) / A) + B) / 12.0;
}

inline double ootf(double y_scene, double gamma = 1.2) {
    return std::pow(y_scene, gamma);
}

}

namespace bt709 {

inline double oetf(double l) {
    if (l < 0.018) {
        return 4.5 * l;
    }
    return 1.099 * std::pow(l, 0.45) - 0.099;
}

inline double eotf(double v) {
    if (v < 0.081) {
        return v / 4.5;
    }
    return std::pow((v + 0.099) / 1.099, 1.0 / 0.45);
}

}

namespace srgb {

inline double oetf(double l) {
    if (l <= 0.0031308) {
        return 12.92 * l;
    }
    return 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

inline double eotf(double v) {
    if (v <= 0.04045) {
        return v / 12.92;
    }
    return std::pow((v + 0.055) / 1.055, 2.4);
}

}

struct ColorMatrix {
    double m[3][3];

    void transform(double r, double g, double b,
                   double& o0, double& o1, double& o2) const {
        o0 = m[0][0] * r + m[0][1] * g + m[0][2] * b;
        o1 = m[1][0] * r + m[1][1] * g + m[1][2] * b;
        o2 = m[2][0] * r + m[2][1] * g + m[2][2] * b;
    }
};

inline ColorMatrix get_rgb_to_ycbcr(uint8_t cicp_matrix) {
    ColorMatrix mat = {};
    switch (cicp_matrix) {
        case 1:
            mat.m[0][0] = 0.2126; mat.m[0][1] = 0.7152; mat.m[0][2] = 0.0722;
            mat.m[1][0] = -0.1146; mat.m[1][1] = -0.3854; mat.m[1][2] = 0.5000;
            mat.m[2][0] = 0.5000; mat.m[2][1] = -0.4542; mat.m[2][2] = -0.0458;
            break;
        case 5:
        case 6:
            mat.m[0][0] = 0.2990; mat.m[0][1] = 0.5870; mat.m[0][2] = 0.1140;
            mat.m[1][0] = -0.1687; mat.m[1][1] = -0.3313; mat.m[1][2] = 0.5000;
            mat.m[2][0] = 0.5000; mat.m[2][1] = -0.4187; mat.m[2][2] = -0.0813;
            break;
        case 9:
            mat.m[0][0] = 0.2627; mat.m[0][1] = 0.6780; mat.m[0][2] = 0.0593;
            mat.m[1][0] = -0.1396; mat.m[1][1] = -0.3604; mat.m[1][2] = 0.5000;
            mat.m[2][0] = 0.5000; mat.m[2][1] = -0.4598; mat.m[2][2] = -0.0402;
            break;
        default:
            mat.m[0][0] = 0.2126; mat.m[0][1] = 0.7152; mat.m[0][2] = 0.0722;
            mat.m[1][0] = -0.1146; mat.m[1][1] = -0.3854; mat.m[1][2] = 0.5000;
            mat.m[2][0] = 0.5000; mat.m[2][1] = -0.4542; mat.m[2][2] = -0.0458;
            break;
    }
    return mat;
}

inline ColorMatrix get_ycbcr_to_rgb(uint8_t cicp_matrix) {
    ColorMatrix mat = {};
    switch (cicp_matrix) {
        case 1:
            mat.m[0][0] = 1.0; mat.m[0][1] = 0.0000; mat.m[0][2] = 1.5748;
            mat.m[1][0] = 1.0; mat.m[1][1] = -0.1873; mat.m[1][2] = -0.4681;
            mat.m[2][0] = 1.0; mat.m[2][1] = 1.8556; mat.m[2][2] = 0.0000;
            break;
        case 5:
        case 6:
            mat.m[0][0] = 1.0; mat.m[0][1] = 0.0000; mat.m[0][2] = 1.4020;
            mat.m[1][0] = 1.0; mat.m[1][1] = -0.3441; mat.m[1][2] = -0.7141;
            mat.m[2][0] = 1.0; mat.m[2][1] = 1.7720; mat.m[2][2] = 0.0000;
            break;
        case 9:
            mat.m[0][0] = 1.0; mat.m[0][1] = 0.0000; mat.m[0][2] = 1.4746;
            mat.m[1][0] = 1.0; mat.m[1][1] = -0.1646; mat.m[1][2] = -0.5714;
            mat.m[2][0] = 1.0; mat.m[2][1] = 1.8814; mat.m[2][2] = 0.0000;
            break;
        default:
            mat.m[0][0] = 1.0; mat.m[0][1] = 0.0000; mat.m[0][2] = 1.5748;
            mat.m[1][0] = 1.0; mat.m[1][1] = -0.1873; mat.m[1][2] = -0.4681;
            mat.m[2][0] = 1.0; mat.m[2][1] = 1.8556; mat.m[2][2] = 0.0000;
            break;
    }
    return mat;
}

inline double apply_eotf(double v, uint8_t cicp_transfer) {
    switch (cicp_transfer) {
        case 1:  return bt709::eotf(v);
        case 13: return srgb::eotf(v);
        case 16: return pq::eotf(v);
        case 18: return hlg::inv_oetf(v);
        default: return srgb::eotf(v);
    }
}

inline double apply_oetf(double v, uint8_t cicp_transfer) {
    switch (cicp_transfer) {
        case 1:  return bt709::oetf(v);
        case 13: return srgb::oetf(v);
        case 16: return pq::oetf(v);
        case 18: return hlg::oetf(v);
        default: return srgb::oetf(v);
    }
}

void rgb_to_ycbcr(int16_t* y_plane, int16_t* cb_plane, int16_t* cr_plane,
                   const uint8_t* rgb, uint32_t width, uint32_t height,
                   uint8_t cicp_matrix, uint8_t bit_depth, bool full_range,
                   bool has_alpha);

void ycbcr_to_rgb(uint8_t* rgb, const int16_t* y_plane,
                   const int16_t* cb_plane, const int16_t* cr_plane,
                   uint32_t width, uint32_t height,
                   uint8_t cicp_matrix, uint8_t bit_depth, bool full_range,
                   bool has_alpha, const int16_t* alpha_plane = nullptr);

void subsample_420(const int16_t* in, int16_t* out,
                    uint32_t width, uint32_t height);

void upsample_420(const int16_t* in, int16_t* out,
                   uint32_t width, uint32_t height);

}

