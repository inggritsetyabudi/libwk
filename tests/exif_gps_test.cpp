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
#include <cstdint>
#include <string>
#include <vector>
#include <wk/wkmeta.hpp>
#include "../src/exif_import.h"

using namespace wk::meta;

namespace {

// Minimal little-endian TIFF holding only a GPS IFD with a date string
// (tag 0x001D, ASCII) and a time rational[3] (tag 0x0007):
//
//   header (8) | IFD0 at 8 (1 entry: GPS IFD -> 26) | GPS IFD at 26
//   (2 entries) | date string at 56 (11 bytes) | rationals at 67 (24 bytes)
std::vector<uint8_t> build_gps_tiff(const std::string& date) {
    std::vector<uint8_t> blob;
    auto u16 = [&blob](uint16_t v) {
        blob.push_back(static_cast<uint8_t>(v & 0xFF));
        blob.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto u32 = [&blob](uint32_t v) {
        blob.push_back(static_cast<uint8_t>(v & 0xFF));
        blob.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        blob.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        blob.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        u16(tag);
        u16(type);
        u32(count);
        u32(value);
    };

    blob.push_back('I');
    blob.push_back('I');
    u16(42);
    u32(8);  // IFD0 offset

    u16(1);  // IFD0: 1 entry
    entry(0x8825, 4, 1, 26);  // GPS IFD pointer -> offset 26
    u32(0);  // next IFD

    u16(2);  // GPS IFD: 2 entries
    entry(0x0007, 5, 3, 67);  // GPS time, rational[3] -> offset 67
    entry(0x001D, 2, 11, 56);  // GPS date, ASCII[11] -> offset 56
    u32(0);  // next IFD

    // Date string at 56, exactly 11 bytes incl. the NUL terminator.
    std::string padded = date;
    padded.resize(11, '\0');
    blob.insert(blob.end(), padded.begin(), padded.end());

    // Time rationals at 67: 10:30:00.
    for (const uint32_t numerator : {10u, 30u, 0u}) {
        u32(numerator);
        u32(1);
    }
    return blob;
}

}  // namespace

TEST(ExifGpsTest, AcceptsValidGpsDate) {
    // Control: "2026:01:15" 10:30:00 UTC must produce a timestamp.
    // 2026-01-15 10:30:00 UTC = 1768473000 seconds since the epoch.
    auto parsed = parse_exif_blob(build_gps_tiff("2026:01:15"));
    ASSERT_TRUE(parsed.has_value());
    auto ts = parsed->get_capture_ts();
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(ts->microseconds, 1768473000000000ULL);
}

TEST(ExifGpsTest, RejectsNonDigitGpsDate) {
    // Before the fix, std::stoi("abcd") threw std::invalid_argument out
    // of gps_timestamp with no handler: importing such EXIF aborted.
    auto parsed = parse_exif_blob(build_gps_tiff("abcd:ef:gh"));
    ASSERT_TRUE(parsed.has_value());
    // A malformed date is dropped leniently (like other bad GPS tags);
    // it must not abort, and no timestamp may be recorded for it.
    EXPECT_FALSE(parsed->get_capture_ts().has_value());
}

TEST(ExifGpsTest, RejectsOutOfRangeGpsDate) {
    // Month 13 / day 45 parse as numbers, but timegm would silently
    // normalize them into a different, wrong date. They must be rejected.
    auto parsed = parse_exif_blob(build_gps_tiff("2026:13:45"));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->get_capture_ts().has_value());
}

TEST(ExifGpsTest, RejectsZeroGpsDate) {
    auto parsed = parse_exif_blob(build_gps_tiff("2026:00:00"));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->get_capture_ts().has_value());
}
