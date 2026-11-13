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
#include "../src/common.h"
#include <cmath>
#include <string>
#include <vector>

using namespace wk::meta;

namespace {

// ---- WKMETA payload builders (raw META chunk payloads) ----

std::vector<uint8_t> entry_bytes(uint8_t ns, uint16_t tag, uint8_t tid,
                                 const std::vector<uint8_t>& value) {
    wk::ByteWriter w;
    w.write_u8(ns);
    w.write_u16(tag);
    w.write_u8(tid);
    w.write_u32(static_cast<uint32_t>(value.size()));
    w.write_bytes(value);
    return w.finish();
}

std::vector<uint8_t> entry_bytes_raw(uint8_t ns, uint16_t tag, uint8_t tid, uint32_t declared_size,
                                     const std::vector<uint8_t>& value) {
    wk::ByteWriter w;
    w.write_u8(ns);
    w.write_u16(tag);
    w.write_u8(tid);
    w.write_u32(declared_size);
    w.write_bytes(value);
    return w.finish();
}

std::vector<uint8_t> meta_payload(const std::vector<std::vector<uint8_t>>& entries) {
    wk::ByteWriter w;
    w.write_u8(0x01);
    w.write_u16(static_cast<uint16_t>(entries.size()));
    for (const auto& e : entries) {
        w.write_bytes(e);
    }
    return w.finish();
}

std::vector<uint8_t> f64_value(double v) {
    wk::ByteWriter w;
    w.write_f64(v);
    return w.finish();
}

std::vector<uint8_t> f32_value(float v) {
    wk::ByteWriter w;
    w.write_f32(v);
    return w.finish();
}

std::vector<uint8_t> u8_value(uint8_t v) {
    wk::ByteWriter w;
    w.write_u8(v);
    return w.finish();
}

std::vector<uint8_t> lstr_value(const std::string& lang, const std::string& text) {
    wk::ByteWriter w;
    w.write_u8(static_cast<uint8_t>(lang.size()));
    w.write_str(lang);
    w.write_str(text);
    return w.finish();
}

}  // namespace

TEST(WkmetaStrictTest, ParsePreservesUnknownTypeEntry) {
    // SPEC section 10 rule 7: unknown WKMETA entries must survive a
    // round-trip byte-identically. Before the fix, the opaque fallback
    // did not move the reader past the unknown entry's value, so the
    // following entry was read from the wrong offset and lost.
    auto unknown = entry_bytes(0x0A, 0x0001, 0x7F, {1, 2, 3, 4});
    auto lat = entry_bytes(0x02, geo::LAT, 0x07, f64_value(48.8566));
    auto payload = meta_payload({unknown, lat});

    auto parsed = MetaBlock::parse(payload);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto lat_out = parsed->get_geo_lat();
    ASSERT_TRUE(lat_out.has_value());
    EXPECT_NEAR(*lat_out, 48.8566, 1e-10);

    EXPECT_EQ(parsed->serialize(), payload);
}

TEST(WkmetaStrictTest, ParseRejectsOversizedEntrySize) {
    // An entry claiming 1000 value bytes when the chunk only holds 4 is
    // truncated, not preservable. Before the fix it was kept as an
    // opaque entry filled with zeroes, and serialize() wrote those
    // zeroes back in place of the original bytes.
    auto entry = entry_bytes_raw(0x0A, 0x0001, 0x0B, 1000, {1, 2, 3, 4});
    auto payload = meta_payload({entry});

    auto parsed = MetaBlock::parse(payload);
    EXPECT_FALSE(parsed.has_value());
}

TEST(WkmetaStrictTest, ParseKeepsFollowingEntryAfterMalformedLstr) {
    // LSTR value_size=2 with lang_len=5: text_len underflows
    // (2-1-5 wraps to a huge uint32). The value is unparseable, but the
    // entry framing (8 header + 2 value bytes) is still well-defined,
    // so the entry must go opaque and the following entry must survive.
    // Before the fix, parsing the language tag consumed bytes of the
    // following entry and desynced the rest of the chunk.
    std::vector<uint8_t> bad_lstr_value = {5, 'e'};
    auto bad = entry_bytes_raw(0x05, content::TITLE, 0x09, 2, bad_lstr_value);
    auto lat = entry_bytes(0x02, geo::LAT, 0x07, f64_value(48.8566));
    auto payload = meta_payload({bad, lat});

    auto parsed = MetaBlock::parse(payload);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto lat_out = parsed->get_geo_lat();
    ASSERT_TRUE(lat_out.has_value());
    EXPECT_NEAR(*lat_out, 48.8566, 1e-10);
}

TEST(WkmetaStrictTest, ParseKeepsFollowingEntryAfterMalformedArray) {
    // ARRAY needs at least 3 value bytes (count + element type).
    // value_size=1 underflows the remaining-bytes computation
    // (1-3 wraps), and the count/type reads would eat the next entry.
    auto bad = entry_bytes_raw(0x05, content::KEYWORDS, 0x0C, 1, {0x03});
    auto lat = entry_bytes(0x02, geo::LAT, 0x07, f64_value(48.8566));
    auto payload = meta_payload({bad, lat});

    auto parsed = MetaBlock::parse(payload);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

    auto lat_out = parsed->get_geo_lat();
    ASSERT_TRUE(lat_out.has_value());
    EXPECT_NEAR(*lat_out, 48.8566, 1e-10);
}

TEST(WkmetaStrictTest, ParseRejectsFixedSizeMismatch) {
    // A UINT8 value is exactly 1 byte (SPEC section 7.4). An entry
    // declaring value_size=5 for it contradicts the type table, so the
    // chunk is malformed. Before the fix the parser read 1 byte, left
    // the reader 4 bytes short of the entry end, and accepted the chunk.
    auto entry = entry_bytes_raw(0x09, rating::STARS, 0x01, 5, {5, 0, 0, 0, 0});
    auto payload = meta_payload({entry});

    auto parsed = MetaBlock::parse(payload);
    EXPECT_FALSE(parsed.has_value());
}

TEST(WkmetaStrictTest, ToXmpIsWellFormed) {
    MetaBlock block;
    block.set(Namespace::Content, content::TITLE,
              LocalizedString{"en", "Fish & Chips <delicious>"});
    block.set(Namespace::Content, content::DESCRIPTION,
              LocalizedString{"en", "A \"quoted\" test"});
    block.set(Namespace::Rights, rights::CREATOR, LocalizedString{"en", "Inggrit Setya Budi"});
    block.set(Namespace::Rights, rights::COPYRIGHT, std::string("Copyright 2026"));
    block.set(Namespace::Rights, rights::LICENSE_SPDX, std::string("CC-BY-4.0"));
    block.set(Namespace::Rights, rights::LICENSE_URL, std::string("https://example.org/license"));

    auto xmp = block.to_xmp();

    // Real namespaces with full URIs -- before the fix the rdf namespace
    // was cut to "http:" and the output contained C++ source fragments.
    EXPECT_TRUE(xmp.find("http://www.w3.org/1999/02/22-rdf-syntax-ns#") != std::string::npos);
    EXPECT_TRUE(xmp.find("http://purl.org/dc/elements/1.1/") != std::string::npos);
    EXPECT_TRUE(xmp.find("http://ns.adobe.com/xap/1.0/rights/") != std::string::npos);
    EXPECT_TRUE(xmp.find("xmp += ") == std::string::npos);
    EXPECT_TRUE(xmp.find("R\"(") == std::string::npos);
    EXPECT_TRUE(xmp.find("xmlns:rdf=\"http:\n") == std::string::npos);

    // Values present, XML-escaped (not JSON-escaped).
    EXPECT_TRUE(xmp.find("Fish &amp; Chips &lt;delicious&gt;") != std::string::npos);
    EXPECT_TRUE(xmp.find("A &quot;quoted&quot; test") != std::string::npos);
    EXPECT_TRUE(xmp.find("Inggrit Setya Budi") != std::string::npos);
    EXPECT_TRUE(xmp.find("CC-BY-4.0") != std::string::npos);
    EXPECT_TRUE(xmp.find("https://example.org/license") != std::string::npos);
    EXPECT_TRUE(xmp.find("Copyright 2026") != std::string::npos);
    EXPECT_TRUE(xmp.find("\\\"") == std::string::npos);

    // Balanced packet structure.
    EXPECT_TRUE(xmp.find("<x:xmpmeta") != std::string::npos);
    EXPECT_TRUE(xmp.find("</x:xmpmeta>") != std::string::npos);
    EXPECT_TRUE(xmp.find("<rdf:RDF") != std::string::npos);
    EXPECT_TRUE(xmp.find("</rdf:RDF>") != std::string::npos);
}

TEST(WkmetaStrictTest, ToXmpRoundTripsThroughFromXmp) {
    MetaBlock block;
    block.set(Namespace::Content, content::TITLE, LocalizedString{"en", "Fish & Chips"});
    block.set(Namespace::Content, content::DESCRIPTION, LocalizedString{"en", "Desc <here>"});
    block.set(Namespace::Rights, rights::CREATOR, LocalizedString{"en", "Inggrit Setya Budi"});
    block.set(Namespace::Rights, rights::COPYRIGHT, std::string("Copyright 2026"));
    block.set(Namespace::Rights, rights::LICENSE_SPDX, std::string("CC-BY-4.0"));
    block.set(Namespace::Rights, rights::LICENSE_URL, std::string("https://example.org/license"));
    block.set(Namespace::Capture, capture::MAKE, std::string("WKCam"));
    block.set(Namespace::Capture, capture::MODEL, std::string("Revival"));
    block.set(Namespace::Capture, capture::SOFTWARE, std::string("wk-tool"));

    auto round_tripped = MetaBlock::from_xmp(block.to_xmp());
    ASSERT_TRUE(round_tripped.has_value()) << round_tripped.error().message;

    auto title = round_tripped->get_title("en");
    ASSERT_TRUE(title.has_value());
    EXPECT_EQ(*title, "Fish & Chips");

    auto desc = round_tripped->get(Namespace::Content, content::DESCRIPTION);
    ASSERT_TRUE(desc.has_value());
    auto* desc_ls = std::get_if<LocalizedString>(*desc);
    ASSERT_NE(desc_ls, nullptr);
    EXPECT_EQ(desc_ls->text, "Desc <here>");

    auto creator = round_tripped->get(Namespace::Rights, rights::CREATOR);
    ASSERT_TRUE(creator.has_value());
    auto* creator_ls = std::get_if<LocalizedString>(*creator);
    ASSERT_NE(creator_ls, nullptr);
    EXPECT_EQ(creator_ls->text, "Inggrit Setya Budi");

    auto license = round_tripped->get_license();
    ASSERT_TRUE(license.has_value());
    EXPECT_EQ(*license, "CC-BY-4.0");

    auto license_url = round_tripped->get(Namespace::Rights, rights::LICENSE_URL);
    ASSERT_TRUE(license_url.has_value());
    auto* url = std::get_if<std::string>(*license_url);
    ASSERT_NE(url, nullptr);
    EXPECT_EQ(*url, "https://example.org/license");

    auto make = round_tripped->get(Namespace::Capture, capture::MAKE);
    ASSERT_TRUE(make.has_value());
    auto* make_str = std::get_if<std::string>(*make);
    ASSERT_NE(make_str, nullptr);
    EXPECT_EQ(*make_str, "WKCam");
}

TEST(WkmetaStrictTest, ToExifRoundTrips) {
    MetaBlock block;
    block.set(Namespace::Capture, capture::MAKE, std::string("WKCam"));
    block.set(Namespace::Capture, capture::MODEL, std::string("Revival"));
    block.set(Namespace::Capture, capture::SOFTWARE, std::string("wk-tool"));
    block.set(Namespace::Capture, capture::ISO, uint32_t(400));
    block.set(Namespace::Geo, geo::LAT, -6.2);
    block.set(Namespace::Geo, geo::LON, 106.825);
    block.set(Namespace::Geo, geo::ALT, 35.0f);
    Timestamp ts;
    ts.microseconds = 1712666096000000ULL;  // 2024-04-09T12:34:56Z
    block.set(Namespace::Geo, geo::CAPTURE_TS, ts);

    // Before the fix to_exif() was a silent empty stub.
    auto exif = block.to_exif();
    ASSERT_FALSE(exif.empty());
    ASSERT_GE(exif.size(), 8u);
    EXPECT_EQ(exif[0], 'I');
    EXPECT_EQ(exif[1], 'I');

    auto round_tripped = MetaBlock::from_exif(exif);
    ASSERT_TRUE(round_tripped.has_value()) << round_tripped.error().message;

    auto make = round_tripped->get(Namespace::Capture, capture::MAKE);
    ASSERT_TRUE(make.has_value());
    EXPECT_EQ(*std::get_if<std::string>(*make), "WKCam");

    auto model = round_tripped->get(Namespace::Capture, capture::MODEL);
    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(*std::get_if<std::string>(*model), "Revival");

    auto software = round_tripped->get(Namespace::Capture, capture::SOFTWARE);
    ASSERT_TRUE(software.has_value());
    EXPECT_EQ(*std::get_if<std::string>(*software), "wk-tool");

    auto iso = round_tripped->get(Namespace::Capture, capture::ISO);
    ASSERT_TRUE(iso.has_value());
    EXPECT_EQ(*std::get_if<uint32_t>(*iso), 400u);

    auto lat = round_tripped->get_geo_lat();
    ASSERT_TRUE(lat.has_value());
    EXPECT_NEAR(*lat, -6.2, 1e-6);

    auto lon = round_tripped->get_geo_lon();
    ASSERT_TRUE(lon.has_value());
    EXPECT_NEAR(*lon, 106.825, 1e-6);

    auto alt = round_tripped->get(Namespace::Geo, geo::ALT);
    ASSERT_TRUE(alt.has_value());
    EXPECT_NEAR(*std::get_if<float>(*alt), 35.0f, 1e-6f);

    auto capture_ts = round_tripped->get_capture_ts();
    ASSERT_TRUE(capture_ts.has_value());
    EXPECT_EQ(capture_ts->microseconds, 1712666096000000ULL);
}

TEST(WkmetaStrictTest, ToExifEmptyBlockReturnsEmpty) {
    MetaBlock block;
    EXPECT_TRUE(block.to_exif().empty());
}

TEST(WkmetaStrictTest, GetRegionsReadsAllFields) {
    MetaBlock block;

    auto person_id = Uuid::generate();

    auto rect = std::make_shared<ValueArray>();
    rect->elements = {Value{0.1f}, Value{0.2f}, Value{0.3f}, Value{0.4f}};

    Struct region_struct;
    region_struct.fields.push_back({Namespace::Region, region::NAME, LocalizedString{"en", "Person 1"}, {}, false});
    region_struct.fields.push_back({Namespace::Region, region::TYPE, uint8_t(1), {}, false});
    region_struct.fields.push_back({Namespace::Region, region::ROLE, std::string("subject"), {}, false});
    region_struct.fields.push_back({Namespace::Region, region::RECT_NORM, Value{rect}, {}, false});
    region_struct.fields.push_back({Namespace::Region, region::CONFIDENCE, float(0.9f), {}, false});
    region_struct.fields.push_back({Namespace::Region, region::PERSON_NAME, LocalizedString{"en", "Ada"}, {}, false});
    region_struct.fields.push_back({Namespace::Region, region::PERSON_ID, person_id, {}, false});
    region_struct.fields.push_back({Namespace::Region, region::OBJECT_CLASS, std::string("person"), {}, false});
    region_struct.fields.push_back({Namespace::Region, region::CREATOR, std::string("alice"), {}, false});

    block.entries.push_back({Namespace::Region, 0x0001, region_struct, {}, false});

    auto regions = block.get_regions();
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].name, "Person 1");
    EXPECT_EQ(regions[0].type, 1);
    EXPECT_EQ(regions[0].role, "subject");
    EXPECT_NEAR(regions[0].x, 0.1f, 1e-6f);
    EXPECT_NEAR(regions[0].y, 0.2f, 1e-6f);
    EXPECT_NEAR(regions[0].w, 0.3f, 1e-6f);
    EXPECT_NEAR(regions[0].h, 0.4f, 1e-6f);
    EXPECT_NEAR(regions[0].confidence, 0.9f, 1e-6f);
    EXPECT_EQ(regions[0].person_name, "Ada");
    EXPECT_EQ(regions[0].person_id, person_id);
    EXPECT_EQ(regions[0].object_class, "person");
    EXPECT_EQ(regions[0].creator, "alice");
}

TEST(WkmetaStrictTest, GetRegionsAcceptsPlainStrings) {
    // String-ish region fields accept both STR and LSTR, like get_title.
    MetaBlock block;

    Struct region_struct;
    region_struct.fields.push_back({Namespace::Region, region::NAME, std::string("Box"), {}, false});
    region_struct.fields.push_back({Namespace::Region, region::PERSON_NAME, std::string("Bob"), {}, false});

    block.entries.push_back({Namespace::Region, 0x0001, region_struct, {}, false});

    auto regions = block.get_regions();
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].name, "Box");
    EXPECT_EQ(regions[0].person_name, "Bob");
}
