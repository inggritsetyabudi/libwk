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

#include "../src/core/common.h"
#include "../src/core/mode_stream.h"

using namespace wk;

TEST(ModeStreamTest, PackAndUnpackModesRoundTrip) {
    const std::vector<PredMode> modes = {
        PredMode::DC,
        PredMode::TM,
        PredMode::D45,
        PredMode::D117,
        PredMode::H
    };

    auto packed = pack_prediction_modes(modes);
    ASSERT_TRUE(packed.has_value()) << packed.error().message;
    EXPECT_EQ(packed->size(), packed_prediction_mode_bytes(modes.size()));

    auto unpacked = unpack_prediction_modes(*packed, modes.size(), "luma");
    ASSERT_TRUE(unpacked.has_value()) << unpacked.error().message;
    EXPECT_EQ(*unpacked, modes);
}

TEST(ModeStreamTest, RejectsInvalidModeNibble) {
    const std::array<uint8_t, 1> packed = {0x0F};
    auto unpacked = unpack_prediction_modes(packed, 1, "luma");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::PredictionError);
}

TEST(ModeStreamTest, RejectsNonZeroPaddingNibble) {
    const std::array<uint8_t, 1> packed = {0x10};
    auto unpacked = unpack_prediction_modes(packed, 1, "luma");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::PredictionError);
}

TEST(ModeStreamTest, ReaderRejectsUnexpectedPackedLength) {
    ByteWriter writer;
    writer.write_u16(1);
    writer.write_u8(0x21);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto unpacked = read_packed_prediction_modes(reader, 3, "luma");
    EXPECT_FALSE(unpacked.has_value());
    EXPECT_EQ(unpacked.error().code, ErrorCode::PredictionError);
}
