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

#include "../src/core/coeff_presence_stream.h"

using namespace wk;

TEST(CoeffPresenceStreamTest, RawPackedRoundTrip) {
    const std::array<uint8_t, 9> presence = {1, 0, 1, 1, 0, 0, 1, 0, 1};

    ByteWriter writer;
    auto written = write_adaptive_coefficient_presence(writer, presence);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffPresenceEncoding::RawPacked));

    ByteReader reader(bytes);
    auto parsed = read_adaptive_coefficient_presence(reader, presence.size(), "lossy");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(*parsed, std::vector<uint8_t>(presence.begin(), presence.end()));
}

TEST(CoeffPresenceStreamTest, AllZeroEncodingRoundTrip) {
    const std::array<uint8_t, 6> presence = {0, 0, 0, 0, 0, 0};

    ByteWriter writer;
    auto written = write_adaptive_coefficient_presence(writer, presence);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffPresenceEncoding::AllZero));

    ByteReader reader(bytes);
    auto parsed = read_adaptive_coefficient_presence(reader, presence.size(), "lossy");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(*parsed, std::vector<uint8_t>(presence.begin(), presence.end()));
}

TEST(CoeffPresenceStreamTest, AllOneEncodingRoundTrip) {
    const std::array<uint8_t, 6> presence = {1, 1, 1, 1, 1, 1};

    ByteWriter writer;
    auto written = write_adaptive_coefficient_presence(writer, presence);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffPresenceEncoding::AllOne));

    ByteReader reader(bytes);
    auto parsed = read_adaptive_coefficient_presence(reader, presence.size(), "lossy");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(*parsed, std::vector<uint8_t>(presence.begin(), presence.end()));
}

TEST(CoeffPresenceStreamTest, RejectsNonZeroPadding) {
    ByteWriter writer;
    writer.write_u8(static_cast<uint8_t>(CoeffPresenceEncoding::RawPacked));
    writer.write_u8(0xFF);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_adaptive_coefficient_presence(reader, 3, "lossy");
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::DecodeFailed);
}
