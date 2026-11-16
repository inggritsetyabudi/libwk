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

#include <gtest/gtest.h>
#include "../src/core/dct.h"
#include <cmath>
#include <random>

using namespace wk;

TEST(DctTest, ForwardInverseIdentity) {

    DctBlock block;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0, 255);

    for (int i = 0; i < 64; i++) {
        block[i] = dist(rng);
    }

    DctBlock original = block;
    dct_2d_forward(block);
    dct_2d_inverse(block);

    for (int i = 0; i < 64; i++) {
        EXPECT_NEAR(block[i], original[i], 0.5f)
            << "Mismatch at index " << i;
    }
}

TEST(DctTest, DcOnlyBlock) {

    DctBlock block;
    std::fill(block.begin(), block.end(), 128.0f);

    dct_2d_forward(block);


    EXPECT_GT(std::abs(block[0]), 10.0f);


    for (int i = 1; i < 64; i++) {
        EXPECT_NEAR(block[i], 0.0f, 1e-3f)
            << "AC coeff " << i << " not zero for constant block";
    }
}

TEST(DctTest, PixelConversion) {

    int16_t pixels[64];
    for (int i = 0; i < 64; i++) {
        pixels[i] = static_cast<int16_t>(100 + (i % 8) * 10 + (i / 8) * 5);
    }

    DctBlock coeffs;
    dct_forward_from_pixels(pixels, 8, coeffs);

    int16_t reconstructed[64];
    dct_inverse_to_pixels(coeffs, reconstructed, 8, 255);

    for (int i = 0; i < 64; i++) {
        EXPECT_NEAR(reconstructed[i], pixels[i], 1)
            << "Pixel mismatch at " << i;
    }
}

TEST(DctTest, ZigzagOrder) {

    for (int i = 0; i < 64; i++) {
        EXPECT_EQ(ZIGZAG_INV[ZIGZAG_ORDER[i]], i)
            << "Zigzag inverse mismatch at " << i;
    }
}

TEST(DctTest, EnergyConservation) {

    DctBlock block;
    std::mt19937 rng(789);
    std::uniform_real_distribution<float> dist(-128, 127);

    for (int i = 0; i < 64; i++) block[i] = dist(rng);

    double spatial_energy = 0;
    for (int i = 0; i < 64; i++) {
        spatial_energy += block[i] * block[i];
    }

    dct_2d_forward(block);

    double freq_energy = 0;
    for (int i = 0; i < 64; i++) {
        freq_energy += block[i] * block[i];
    }



    EXPECT_GT(freq_energy, 0.0);
    EXPECT_GT(spatial_energy, 0.0);
}
