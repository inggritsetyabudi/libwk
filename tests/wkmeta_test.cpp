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
#include <wk/wkmeta.hpp>
#include "../src/core/common.h"
#include "../src/core/exif_import.h"
#include <cmath>
#include <string>
#include <vector>

using namespace wk::meta;

namespace {

void write_ifd_entry(wk::ByteWriter& writer, uint16_t tag, uint16_t type, uint32_t count, uint32_t value_or_offset) {
    writer.write_u16(tag);
    writer.write_u16(type);
    writer.write_u32(count);
    writer.write_u32(value_or_offset);
}

void write_rational(wk::ByteWriter& writer, uint32_t numerator, uint32_t denominator) {
    writer.write_u32(numerator);
    writer.write_u32(denominator);
}

std::vector<uint8_t> make_test_exif_blob() {
    constexpr uint16_t kAscii = 2;
    constexpr uint16_t kShort = 3;
    constexpr uint16_t kLong = 4;
    constexpr uint16_t kRational = 5;
    constexpr uint16_t kByte = 1;

    constexpr uint16_t kTagMake = 0x010F;
    constexpr uint16_t kTagModel = 0x0110;
    constexpr uint16_t kTagSoftware = 0x0131;
    constexpr uint16_t kTagExifIfd = 0x8769;
    constexpr uint16_t kTagGpsIfd = 0x8825;
    constexpr uint16_t kTagIso = 0x8827;

    constexpr uint16_t kGpsLatRef = 0x0001;
    constexpr uint16_t kGpsLat = 0x0002;
    constexpr uint16_t kGpsLonRef = 0x0003;
    constexpr uint16_t kGpsLon = 0x0004;
    constexpr uint16_t kGpsAltRef = 0x0005;
    constexpr uint16_t kGpsAlt = 0x0006;
    constexpr uint16_t kGpsTime = 0x0007;
    constexpr uint16_t kGpsDate = 0x001D;

    constexpr uint32_t ifd0_offset = 8;
    constexpr uint32_t make_offset = 74;
    constexpr uint32_t model_offset = 80;
    constexpr uint32_t software_offset = 88;
    constexpr uint32_t exif_ifd_offset = 96;
    constexpr uint32_t gps_ifd_offset = 114;
    constexpr uint32_t lat_offset = 216;
    constexpr uint32_t lon_offset = 240;
    constexpr uint32_t alt_offset = 264;
    constexpr uint32_t time_offset = 272;
    constexpr uint32_t date_offset = 296;

    wk::ByteWriter writer;
    writer.write_u8('I');
    writer.write_u8('I');
    writer.write_u16(42);
    writer.write_u32(ifd0_offset);

    writer.write_u16(5);
    write_ifd_entry(writer, kTagMake, kAscii, 6, make_offset);
    write_ifd_entry(writer, kTagModel, kAscii, 8, model_offset);
    write_ifd_entry(writer, kTagSoftware, kAscii, 8, software_offset);
    write_ifd_entry(writer, kTagExifIfd, kLong, 1, exif_ifd_offset);
    write_ifd_entry(writer, kTagGpsIfd, kLong, 1, gps_ifd_offset);
    writer.write_u32(0);

    writer.write_str("WKCam");
    writer.write_u8(0);
    writer.write_str("Revival");
    writer.write_u8(0);
    writer.write_str("wk-tool");
    writer.write_u8(0);

    writer.write_u16(1);
    write_ifd_entry(writer, kTagIso, kShort, 1, 400);
    writer.write_u32(0);

    writer.write_u16(8);
    write_ifd_entry(writer, kGpsLatRef, kAscii, 2, static_cast<uint32_t>('S'));
    write_ifd_entry(writer, kGpsLat, kRational, 3, lat_offset);
    write_ifd_entry(writer, kGpsLonRef, kAscii, 2, static_cast<uint32_t>('E'));
    write_ifd_entry(writer, kGpsLon, kRational, 3, lon_offset);
    write_ifd_entry(writer, kGpsAltRef, kByte, 1, 0);
    write_ifd_entry(writer, kGpsAlt, kRational, 1, alt_offset);
    write_ifd_entry(writer, kGpsTime, kRational, 3, time_offset);
    write_ifd_entry(writer, kGpsDate, kAscii, 11, date_offset);
    writer.write_u32(0);

    write_rational(writer, 6, 1);
    write_rational(writer, 12, 1);
    write_rational(writer, 0, 1);

    write_rational(writer, 106, 1);
    write_rational(writer, 49, 1);
    write_rational(writer, 30, 1);

    write_rational(writer, 35, 1);

    write_rational(writer, 12, 1);
    write_rational(writer, 34, 1);
    write_rational(writer, 56, 1);

    writer.write_str("2024:04:09");
    writer.write_u8(0);

    return writer.finish();
}

std::vector<uint8_t> make_test_exif_jpeg() {
    const auto exif = make_test_exif_blob();
    const uint16_t app1_size = static_cast<uint16_t>(2 + 6 + exif.size());

    std::vector<uint8_t> jpeg;
    jpeg.reserve(static_cast<size_t>(app1_size) + 6);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD8);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xE1);
    jpeg.push_back(static_cast<uint8_t>((app1_size >> 8) & 0xFF));
    jpeg.push_back(static_cast<uint8_t>(app1_size & 0xFF));
    jpeg.insert(jpeg.end(), {'E', 'x', 'i', 'f', 0, 0});
    jpeg.insert(jpeg.end(), exif.begin(), exif.end());
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

}

TEST(WkmetaTest, SetAndGet) {
    MetaBlock block;

    block.set(Namespace::Geo, geo::LAT, 1.2345);
    block.set(Namespace::Geo, geo::LON, 103.8198);

    auto lat = block.get_geo_lat();
    ASSERT_TRUE(lat.has_value());
    EXPECT_NEAR(*lat, 1.2345, 1e-10);

    auto lon = block.get_geo_lon();
    ASSERT_TRUE(lon.has_value());
    EXPECT_NEAR(*lon, 103.8198, 1e-10);
}

TEST(WkmetaTest, SerializeAndParse) {
    MetaBlock original;
    original.set(Namespace::Geo, geo::LAT, 48.8566);
    original.set(Namespace::Geo, geo::LON, 2.3522);
    original.set(Namespace::Content, content::TITLE, LocalizedString{"en", "Eiffel Tower"});
    original.set(Namespace::Rights, rights::LICENSE_SPDX, std::string("CC-BY-4.0"));
    original.set(Namespace::Rating, rating::STARS, uint8_t(5));
    original.set(Namespace::Capture, capture::ISO, uint32_t(400));

    auto bytes = original.serialize();
    EXPECT_GT(bytes.size(), 0u);

    auto parsed = MetaBlock::parse(bytes);
    ASSERT_TRUE(parsed.has_value());

    auto lat = parsed->get_geo_lat();
    ASSERT_TRUE(lat.has_value());
    EXPECT_NEAR(*lat, 48.8566, 1e-10);

    auto lon = parsed->get_geo_lon();
    ASSERT_TRUE(lon.has_value());
    EXPECT_NEAR(*lon, 2.3522, 1e-10);

    auto title = parsed->get_title("en");
    ASSERT_TRUE(title.has_value());
    EXPECT_EQ(*title, "Eiffel Tower");

    auto license = parsed->get_license();
    ASSERT_TRUE(license.has_value());
    EXPECT_EQ(*license, "CC-BY-4.0");
}

TEST(WkmetaTest, FromExifImportsCommonFields) {
    const auto exif = make_test_exif_blob();
    auto block = MetaBlock::from_exif(exif);
    ASSERT_TRUE(block.has_value()) << block.error().message;

    auto make = block->get(Namespace::Capture, capture::MAKE);
    ASSERT_TRUE(make.has_value());
    ASSERT_NE(std::get_if<std::string>(*make), nullptr);
    EXPECT_EQ(*std::get_if<std::string>(*make), "WKCam");

    auto model = block->get(Namespace::Capture, capture::MODEL);
    ASSERT_TRUE(model.has_value());
    ASSERT_NE(std::get_if<std::string>(*model), nullptr);
    EXPECT_EQ(*std::get_if<std::string>(*model), "Revival");

    auto software = block->get(Namespace::Capture, capture::SOFTWARE);
    ASSERT_TRUE(software.has_value());
    ASSERT_NE(std::get_if<std::string>(*software), nullptr);
    EXPECT_EQ(*std::get_if<std::string>(*software), "wk-tool");

    auto iso = block->get(Namespace::Capture, capture::ISO);
    ASSERT_TRUE(iso.has_value());
    ASSERT_NE(std::get_if<uint32_t>(*iso), nullptr);
    EXPECT_EQ(*std::get_if<uint32_t>(*iso), 400u);

    auto lat = block->get_geo_lat();
    ASSERT_TRUE(lat.has_value());
    EXPECT_NEAR(*lat, -6.2, 1e-6);

    auto lon = block->get_geo_lon();
    ASSERT_TRUE(lon.has_value());
    EXPECT_NEAR(*lon, 106.825, 1e-6);

    auto alt = block->get(Namespace::Geo, geo::ALT);
    ASSERT_TRUE(alt.has_value());
    ASSERT_NE(std::get_if<float>(*alt), nullptr);
    EXPECT_NEAR(*std::get_if<float>(*alt), 35.0f, 1e-6f);

    auto capture_ts = block->get_capture_ts();
    ASSERT_TRUE(capture_ts.has_value());
    EXPECT_EQ(capture_ts->to_iso8601().substr(0, 19), "2024-04-09T12:34:56");
}

TEST(WkmetaTest, ExtractExifBlobFromJpegWrapper) {
    const auto expected = make_test_exif_blob();
    const auto wrapped_jpeg = make_test_exif_jpeg();

    auto extracted = extract_exif_blob_from_bytes(wrapped_jpeg);
    ASSERT_TRUE(extracted.has_value()) << extracted.error().message;
    EXPECT_EQ(*extracted, expected);
}

TEST(WkmetaTest, RemoveEntry) {
    MetaBlock block;
    block.set(Namespace::Geo, geo::LAT, 1.0);
    block.set(Namespace::Geo, geo::LON, 2.0);

    EXPECT_TRUE(block.remove(Namespace::Geo, geo::LAT));
    EXPECT_FALSE(block.get_geo_lat().has_value());
    EXPECT_TRUE(block.get_geo_lon().has_value());
}

TEST(WkmetaTest, OverwriteEntry) {
    MetaBlock block;
    block.set(Namespace::Rating, rating::STARS, uint8_t(3));
    block.set(Namespace::Rating, rating::STARS, uint8_t(5));

    auto value = block.get(Namespace::Rating, rating::STARS);
    ASSERT_TRUE(value.has_value());
    auto* star = std::get_if<uint8_t>(*value);
    ASSERT_NE(star, nullptr);
    EXPECT_EQ(*star, 5);
}

TEST(WkmetaTest, Timestamp) {
    Timestamp ts;
    ts.microseconds = 1609459200000000ULL;

    auto iso = ts.to_iso8601();
    EXPECT_EQ(iso.substr(0, 19), "2021-01-01T00:00:00");

    auto parsed = Timestamp::from_iso8601("2021-01-01T00:00:00Z");
    EXPECT_EQ(parsed.microseconds, ts.microseconds);
}

TEST(WkmetaTest, TimestampWithMicroseconds) {
    Timestamp ts;
    ts.microseconds = 1609459200123456ULL;

    auto iso = ts.to_iso8601();
    EXPECT_TRUE(iso.find(".123456") != std::string::npos || iso.find("123456") != std::string::npos);
}

TEST(WkmetaTest, Uuid) {
    auto uuid = Uuid::generate();
    auto str = uuid.to_string();

    EXPECT_EQ(str.size(), 36u);
    EXPECT_EQ(uuid.bytes[6] & 0xF0, 0x40);
    EXPECT_EQ(uuid.bytes[8] & 0xC0, 0x80);

    auto parsed = Uuid::from_string(str);
    EXPECT_EQ(parsed, uuid);
}

TEST(WkmetaTest, JsonExport) {
    MetaBlock block;
    block.set(Namespace::Geo, geo::LAT, 1.234);
    block.set(Namespace::Content, content::TITLE, LocalizedString{"en", "Test"});
    block.set(Namespace::Rights, rights::LICENSE_SPDX, std::string("MIT"));

    auto json = block.to_json();
    EXPECT_FALSE(json.empty());
    EXPECT_TRUE(json.find("geo") != std::string::npos);
    EXPECT_TRUE(json.find("content") != std::string::npos);
    EXPECT_TRUE(json.find("rights") != std::string::npos);
    EXPECT_TRUE(json.find("1.234") != std::string::npos);
    EXPECT_TRUE(json.find("Test") != std::string::npos);
    EXPECT_TRUE(json.find("MIT") != std::string::npos);
}

TEST(WkmetaTest, RationalType) {
    MetaBlock block;
    block.set(Namespace::Capture, capture::FOCAL_LEN, Rational{50, 1});

    auto value = block.get(Namespace::Capture, capture::FOCAL_LEN);
    ASSERT_TRUE(value.has_value());
    auto* rational = std::get_if<Rational>(*value);
    ASSERT_NE(rational, nullptr);
    EXPECT_EQ(rational->numerator, 50);
    EXPECT_EQ(rational->denominator, 1);
    EXPECT_NEAR(rational->to_double(), 50.0, 1e-10);
}

TEST(WkmetaTest, LocalizedString) {
    MetaBlock block;
    block.set(Namespace::Content, content::TITLE, LocalizedString{"id", "Menara Eiffel"});

    auto title_id = block.get_title("id");
    ASSERT_TRUE(title_id.has_value());
    EXPECT_EQ(*title_id, "Menara Eiffel");
}

TEST(WkmetaTest, RegionAnnotation) {
    MetaBlock block;

    Struct region_struct;
    region_struct.fields.push_back({Namespace::Region, region::NAME, LocalizedString{"en", "Person 1"}, {}, false});
    region_struct.fields.push_back({Namespace::Region, region::TYPE, uint8_t(0), {}, false});
    region_struct.fields.push_back({Namespace::Region, region::CONFIDENCE, float(0.95f), {}, false});

    block.entries.push_back({Namespace::Region, 0x0001, region_struct, {}, false});

    auto regions = block.get_regions();
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].name, "Person 1");
    EXPECT_EQ(regions[0].type, 0);
    EXPECT_NEAR(regions[0].confidence, 0.95f, 1e-6f);
}

TEST(WkmetaTest, EmptyParseSafety) {
    auto result = MetaBlock::parse({});
    EXPECT_FALSE(result.has_value());
}

TEST(WkmetaTest, MalformedInputSafety) {
    uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    auto result = MetaBlock::parse(garbage);
    EXPECT_FALSE(result.has_value());
}
