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
#include "../src/core/container.h"
#include "../src/core/meta_compress.h"
#include <cstring>
#include <string>

using namespace wk;

namespace
{

FrameHeader make_test_header()
{
    FrameHeader hdr;
    hdr.width = 640;
    hdr.height = 480;
    hdr.bit_depth = 8;
    hdr.cicp_primaries = 1;
    hdr.cicp_transfer = 1;
    hdr.cicp_matrix = 1;
    hdr.flags = FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    hdr.tile_size_log2 = 9;
    return hdr;
}

void append_chunk(ByteWriter &writer, const char type[4], uint8_t flags,
                  std::span<const uint8_t> payload)
{
    writer.write_bytes({reinterpret_cast<const uint8_t *>(type), 4});
    writer.write_u8(flags);
    writer.write_u32(static_cast<uint32_t>(payload.size()));
    writer.write_bytes(payload);
}

struct FoundMeta
{
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
};

// Walk the chunks of a serialized container and return the META chunk
// (flags + payload), or nullopt when there is none.
std::optional<FoundMeta> find_meta_chunk(const std::vector<uint8_t> &bytes)
{
    size_t pos = 7; // file header: magic (5) + version (2)
    while (pos + 9 <= bytes.size())
    {
        const bool is_meta = std::memcmp(bytes.data() + pos, CHUNK_WKMETA, 4) == 0;
        const uint8_t flags = bytes[pos + 4];
        const uint32_t size = static_cast<uint32_t>(bytes[pos + 5]) |
                              (static_cast<uint32_t>(bytes[pos + 6]) << 8) |
                              (static_cast<uint32_t>(bytes[pos + 7]) << 16) |
                              (static_cast<uint32_t>(bytes[pos + 8]) << 24);
        if (pos + 9 + size > bytes.size())
            return std::nullopt;
        if (is_meta)
        {
            FoundMeta found;
            found.flags = flags;
            found.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos + 9),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(pos + 9 + size));
            return found;
        }
        pos += 9 + size;
    }
    return std::nullopt;
}

// A realistic large META block: a title, a ~700-char description built
// from distinct sentences, a creator, and a copyright line. The probe
// measured this shape at 602 B saved (42.82%) with varied text.
meta::MetaBlock make_large_meta()
{
    meta::MetaBlock meta;
    const char *sentences[] = {
        "Pemandangan danau di antara pegunungan saat senja.",
        "Cahaya matahari sore memantul di permukaan air yang tenang.",
        "Perahu nelayan kecil berlayar perlahan menuju dermaga kayu.",
        "Awan tipis bergerak di atas puncak gunung yang berkabut.",
        "Burung-burung terbang pulang melintasi langit jingga.",
        "Angin sepoi-sepoi menggoyangkan dedaunan di tepi danau.",
        "Suara jangkrik mulai terdengar dari balik semak belukar.",
        "Jalan setapak berbatu menurun menuju perkampungan nelayan.",
        "Anak-anak bermain layang-layang di lapangan dekat pantai.",
        "Nelayan tua itu memperbaiki jaringnya di bawah pohon kelapa.",
        "Aroma masakan dari warung kecil tercium sampai ke dermaga.",
        "Bulan sabit muncul perlahan di antara pepohonan pinus.",
    };
    std::string desc;
    for (const char *s : sentences)
    {
        desc += s;
        desc += ' ';
    }
    EXPECT_TRUE(meta.set(meta::Namespace::Content, meta::content::TITLE,
                         std::string("Foto uji tekstur halus"))
                    .has_value());
    EXPECT_TRUE(meta.set(meta::Namespace::Content, meta::content::DESCRIPTION, desc)
                    .has_value());
    EXPECT_TRUE(meta.set(meta::Namespace::Content, meta::content::KEYWORDS,
                         std::string("danau, pegunungan, senja, nelayan, perahu"))
                    .has_value());
    EXPECT_TRUE(meta.set(meta::Namespace::Rights, meta::rights::CREATOR,
                         std::string("Inggrit Setya Budi"))
                    .has_value());
    return meta;
}

// The baseline lossy payload: a single quality-score entry, 15 bytes.
meta::MetaBlock make_quality_score_meta()
{
    meta::MetaBlock meta;
    EXPECT_TRUE(meta.set(meta::Namespace::Rating, meta::rating::QUALITY_SCORE, 42.5f)
                    .has_value());
    return meta;
}

WkFile make_file_with_meta(meta::MetaBlock meta)
{
    WkFile file;
    file.header = make_test_header();
    file.header.flags |= FHDR_FLAG_HAS_WKMETA;
    file.metadata = std::move(meta);
    return file;
}

// Build a raw container holding exactly one META chunk with the given
// flags and payload (no tiles): enough for parse_container's META path.
std::vector<uint8_t> build_raw_meta_container(uint8_t meta_flags,
                                              std::span<const uint8_t> meta_payload)
{
    ByteWriter w;
    w.write_bytes({WK_MAGIC, 5});
    w.write_u16(WK_VERSION);
    FrameHeader hdr = make_test_header();
    hdr.flags |= FHDR_FLAG_HAS_WKMETA;
    append_chunk(w, CHUNK_FHDR, 0, serialize_fhdr(hdr));
    append_chunk(w, CHUNK_WKMETA, meta_flags, meta_payload);
    append_chunk(w, CHUNK_FEND, 0, {});
    return w.finish();
}

} // namespace

TEST(MetaCompressTest, CompressSmallPayloadReturnsNullopt)
{
    auto meta = make_quality_score_meta();
    auto raw = meta.serialize();
    ASSERT_EQ(raw.size(), 15u);
    EXPECT_FALSE(meta::compress_meta_payload(raw).has_value());
}

TEST(MetaCompressTest, CompressRepetitivePayloadRoundTrips)
{
    std::vector<uint8_t> raw(1000, static_cast<uint8_t>('X'));
    auto compressed = meta::compress_meta_payload(raw);
    ASSERT_TRUE(compressed.has_value());
    EXPECT_LT(compressed->size(), raw.size());
    auto back = meta::decompress_meta_payload(*compressed);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, raw);
}

TEST(MetaCompressTest, DecompressRejectsGarbage)
{
    const std::vector<uint8_t> garbage{0x78, 0x9C, 0x01, 0x02, 0x03};
    auto back = meta::decompress_meta_payload(garbage);
    EXPECT_FALSE(back.has_value());
}

TEST(MetaCompressTest, DecompressRejectsTrailingGarbage)
{
    std::vector<uint8_t> raw(200, static_cast<uint8_t>('A'));
    auto compressed = meta::compress_meta_payload(raw);
    ASSERT_TRUE(compressed.has_value());
    compressed->push_back(0x00); // trailing byte after the zlib stream
    auto back = meta::decompress_meta_payload(*compressed);
    EXPECT_FALSE(back.has_value());
}

TEST(MetaCompressTest, DecompressRejectsBomb)
{
    // 17 MiB of zeros compresses to ~130 KB but expands past the 16 MiB
    // cap: the decoder must fail instead of allocating the full output.
    std::vector<uint8_t> raw(17 * 1024 * 1024, 0);
    auto compressed = meta::compress_meta_payload(raw);
    ASSERT_TRUE(compressed.has_value());
    auto back = meta::decompress_meta_payload(*compressed);
    EXPECT_FALSE(back.has_value());
}

TEST(MetaCompressTest, LargeMetaIsCompressedAndRoundTrips)
{
    auto meta = make_large_meta();
    const auto raw = meta.serialize();
    ASSERT_GT(raw.size(), 500u);

    WkFile file = make_file_with_meta(std::move(meta));
    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto found = find_meta_chunk(*written);
    ASSERT_TRUE(found.has_value());
    EXPECT_NE(found->flags & CHUNK_FLAG_META_COMPRESSED, 0u);
    EXPECT_LT(found->payload.size(), raw.size());

    auto parsed = parse_container(*written);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->metadata.has_value());
    EXPECT_EQ(parsed->metadata->serialize(), raw);
}

TEST(MetaCompressTest, SmallMetaStaysRaw)
{
    auto meta = make_quality_score_meta();
    const auto raw = meta.serialize();

    WkFile file = make_file_with_meta(std::move(meta));
    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto found = find_meta_chunk(*written);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->flags & CHUNK_FLAG_META_COMPRESSED, 0u);
    EXPECT_EQ(found->payload, raw);

    auto parsed = parse_container(*written);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->metadata.has_value());
    EXPECT_EQ(parsed->metadata->serialize(), raw);
}

TEST(MetaCompressTest, OpaqueEntrySurvivesCompression)
{
    auto meta = make_large_meta();

    // An entry with an unknown type id (0xFF): the parser preserves it
    // verbatim as opaque (conformance rule 7). Give it a long value so
    // the payload still compresses.
    meta::Entry opaque;
    opaque.ns = meta::Namespace::Custom;
    opaque.tag = 0x00FF;
    opaque.is_opaque = true;
    {
        ByteWriter w;
        w.write_u8(static_cast<uint8_t>(meta::Namespace::Custom));
        w.write_u16(0x00FF);
        w.write_u8(0xFF); // unknown type id
        const std::vector<uint8_t> value(600, static_cast<uint8_t>('R'));
        w.write_u32(static_cast<uint32_t>(value.size()));
        w.write_bytes(value);
        opaque.opaque_data = w.finish();
    }
    meta.entries.push_back(std::move(opaque));
    const auto raw = meta.serialize();

    WkFile file = make_file_with_meta(std::move(meta));
    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto found = find_meta_chunk(*written);
    ASSERT_TRUE(found.has_value());
    EXPECT_NE(found->flags & CHUNK_FLAG_META_COMPRESSED, 0u);

    auto parsed = parse_container(*written);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->metadata.has_value());
    EXPECT_EQ(parsed->metadata->serialize(), raw);
    ASSERT_EQ(parsed->metadata->entries.size(), 5u);
    const auto &back = parsed->metadata->entries.back();
    EXPECT_TRUE(back.is_opaque);
    EXPECT_EQ(back.opaque_data.size(), 608u);
}

TEST(MetaCompressTest, CorruptCompressedMetaRejected)
{
    const std::vector<uint8_t> garbage{0x78, 0x9C, 0x01, 0x02, 0x03};
    auto bytes = build_raw_meta_container(
        static_cast<uint8_t>(CHUNK_FLAG_OPTIONAL | CHUNK_FLAG_META_ONLY |
                             CHUNK_FLAG_META_COMPRESSED),
        garbage);
    auto parsed = parse_container(bytes);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::MetaParseError);
}

TEST(MetaCompressTest, BombMetaRejected)
{
    std::vector<uint8_t> raw(17 * 1024 * 1024, 0);
    auto compressed = meta::compress_meta_payload(raw);
    ASSERT_TRUE(compressed.has_value());
    auto bytes = build_raw_meta_container(
        static_cast<uint8_t>(CHUNK_FLAG_OPTIONAL | CHUNK_FLAG_META_ONLY |
                             CHUNK_FLAG_META_COMPRESSED),
        *compressed);
    auto parsed = parse_container(bytes);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::MetaParseError);
}

TEST(MetaCompressTest, OldDecoderRejectsCompressedMeta)
{
    // The main-era decoder ignores the chunk flags byte and feeds the
    // raw chunk payload to MetaBlock::parse: a zlib stream (first byte
    // 0x78, never the 0x01 wkmeta version) must fail there, so the old
    // decoder rejects the file with a clean error instead of misreading
    // it.
    auto meta = make_large_meta();
    WkFile file = make_file_with_meta(std::move(meta));
    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto found = find_meta_chunk(*written);
    ASSERT_TRUE(found.has_value());
    ASSERT_NE(found->flags & CHUNK_FLAG_META_COMPRESSED, 0u);
    ASSERT_FALSE(found->payload.empty());
    EXPECT_EQ(found->payload[0], 0x78);

    auto old_style = meta::MetaBlock::parse(found->payload);
    EXPECT_FALSE(old_style.has_value());
}
