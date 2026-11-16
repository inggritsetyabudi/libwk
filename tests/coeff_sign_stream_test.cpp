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

#include "../src/core/coeff_sign_stream.h"

using namespace wk;

TEST(CoeffSignStreamTest, PackAndUnpackSignsRoundTrip) {
    const std::array<uint8_t, 10> signs = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0};

    auto packed = pack_coefficient_signs(signs);
    ASSERT_TRUE(packed.has_value()) << packed.error().message;
    EXPECT_EQ(packed->size(), packed_coefficient_sign_bytes(signs.size()));

    auto unpacked = unpack_coefficient_signs(*packed, signs.size(), "lossy");
    ASSERT_TRUE(unpacked.has_value()) << unpacked.error().message;
    EXPECT_TRUE(std::equal(unpacked->begin(), unpacked->end(), signs.begin()));
}

TEST(CoeffSignStreamTest, RejectsInvalidSignValue) {
    const std::array<uint8_t, 1> signs = {2};
    auto packed = pack_coefficient_signs(signs);
    EXPECT_FALSE(packed.has_value());
    EXPECT_EQ(packed.error().code, ErrorCode::InvalidParameter);
}

TEST(CoeffSignStreamTest, RejectsUnexpectedPackedLength) {
    const std::array<uint8_t, 1> packed = {0x01};
    auto unpacked = unpack_coefficient_signs(packed, 9, "lossy");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::DecodeFailed);
}

TEST(CoeffSignStreamTest, RejectsNonZeroPaddingBits) {
    const std::array<uint8_t, 2> packed = {0x01, 0x80};
    auto unpacked = unpack_coefficient_signs(packed, 9, "lossy");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::DecodeFailed);
}

TEST(CoeffSignStreamTest, PackAndUnpackSignModesRoundTrip) {
    const std::array<uint8_t, 6> modes = {
        kCoefficientSignModeRawPacked,
        kCoefficientSignModeAllPositive,
        kCoefficientSignModeAllNegative,
        kCoefficientSignModeRawPacked,
        kCoefficientSignModeAllNegative,
        kCoefficientSignModeAllPositive,
    };

    auto packed = pack_coefficient_sign_modes(modes);
    ASSERT_TRUE(packed.has_value()) << packed.error().message;
    EXPECT_EQ(packed->size(), packed_coefficient_sign_mode_bytes(modes.size()));

    auto unpacked = unpack_coefficient_sign_modes(*packed, modes.size(), "lossy");
    ASSERT_TRUE(unpacked.has_value()) << unpacked.error().message;
    EXPECT_TRUE(std::equal(unpacked->begin(), unpacked->end(), modes.begin()));
}

TEST(CoeffSignStreamTest, RejectsInvalidSignModeValue) {
    const std::array<uint8_t, 1> modes = {3};
    auto packed = pack_coefficient_sign_modes(modes);
    EXPECT_FALSE(packed.has_value());
    EXPECT_EQ(packed.error().code, ErrorCode::InvalidParameter);
}

TEST(CoeffSignStreamTest, RejectsReservedSignModeOnDecode) {
    const std::array<uint8_t, 1> packed = {0x03};
    auto unpacked = unpack_coefficient_sign_modes(packed, 1, "lossy");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::DecodeFailed);
}

TEST(CoeffSignStreamTest, RejectsNonZeroSignModePaddingBits) {
    const std::array<uint8_t, 1> packed = {0x40};
    auto unpacked = unpack_coefficient_sign_modes(packed, 1, "lossy");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::DecodeFailed);
}
