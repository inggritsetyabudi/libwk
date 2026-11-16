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


#include "dct.h"
#include <algorithm>
#include <cmath>

namespace wk {

namespace {

constexpr float kPi = 3.14159265358979323846f;

const std::array<std::array<float, 8>, 8> kCosineTable = [] {
    std::array<std::array<float, 8>, 8> table{};
    for (int k = 0; k < 8; ++k) {
        for (int n = 0; n < 8; ++n) {
            table[k][n] = std::cos(kPi * static_cast<float>((2 * n + 1) * k) / 16.0f);
        }
    }
    return table;
}();

constexpr float alpha(int index) {
    return index == 0 ? 0.35355339059327376220f : 0.5f;
}

}

void dct_1d_forward(float* data) {
    float tmp[8];
    for (int k = 0; k < 8; ++k) {
        float sum = 0.0f;
        for (int n = 0; n < 8; ++n) {
            sum += data[n] * kCosineTable[k][n];
        }
        tmp[k] = alpha(k) * sum;
    }

    for (int i = 0; i < 8; ++i) {
        data[i] = tmp[i];
    }
}

void dct_1d_inverse(float* data) {
    float tmp[8];
    for (int n = 0; n < 8; ++n) {
        float sum = 0.0f;
        for (int k = 0; k < 8; ++k) {
            sum += alpha(k) * data[k] * kCosineTable[k][n];
        }
        tmp[n] = sum;
    }

    for (int i = 0; i < 8; ++i) {
        data[i] = tmp[i];
    }
}

void dct_2d_forward(DctBlock& block) {
    for (int row = 0; row < 8; ++row) {
        dct_1d_forward(&block[row * 8]);
    }

    float column[8];
    for (int col = 0; col < 8; ++col) {
        for (int row = 0; row < 8; ++row) {
            column[row] = block[row * 8 + col];
        }
        dct_1d_forward(column);
        for (int row = 0; row < 8; ++row) {
            block[row * 8 + col] = column[row];
        }
    }
}

void dct_2d_inverse(DctBlock& block) {
    float column[8];
    for (int col = 0; col < 8; ++col) {
        for (int row = 0; row < 8; ++row) {
            column[row] = block[row * 8 + col];
        }
        dct_1d_inverse(column);
        for (int row = 0; row < 8; ++row) {
            block[row * 8 + col] = column[row];
        }
    }

    for (int row = 0; row < 8; ++row) {
        dct_1d_inverse(&block[row * 8]);
    }
}

void dct_forward_from_pixels(const int16_t* pixels, int stride, DctBlock& out) {
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            out[row * 8 + col] = static_cast<float>(pixels[row * stride + col]);
        }
    }
    dct_2d_forward(out);
}

void dct_inverse_to_pixels(const DctBlock& block, int16_t* pixels,
                            int stride, int16_t max_val) {
    DctBlock tmp = block;
    dct_2d_inverse(tmp);

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int16_t value = static_cast<int16_t>(std::round(tmp[row * 8 + col]));
            pixels[row * stride + col] =
                std::clamp(value, static_cast<int16_t>(0), max_val);
        }
    }
}

}
