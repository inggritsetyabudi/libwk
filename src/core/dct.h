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

namespace wk {

constexpr std::array<int, 64> ZIGZAG_ORDER = {{
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
}};

constexpr std::array<int, 64> ZIGZAG_INV = [] {
    std::array<int, 64> inv{};
    for (int i = 0; i < 64; ++i) {
        inv[ZIGZAG_ORDER[i]] = i;
    }
    return inv;
}();

using DctBlock = std::array<float, 64>;
using DctBlockI16 = std::array<int16_t, 64>;


void dct_1d_forward(float* data);


void dct_2d_forward(DctBlock& block);


void dct_forward_from_pixels(const int16_t* pixels, int stride, DctBlock& out);


void dct_1d_inverse(float* data);


void dct_2d_inverse(DctBlock& block);


void dct_inverse_to_pixels(const DctBlock& block, int16_t* pixels,
                            int stride, int16_t max_val);

}
