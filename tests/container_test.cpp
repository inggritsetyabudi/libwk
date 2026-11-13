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
#include "../src/container.h"
#include <cstring>

using namespace wk;

namespace {

Chunk make_tile_chunk(uint32_t claimed_size, uint32_t actual_size) {
    Chunk tile_chunk;
    std::memcpy(tile_chunk.type, CHUNK_TILE, 4);

    TileHeader header;
    header.tile_x = 0;
    header.tile_y = 0;
    header.layer_flags = TILE_HAS_BASE;
    header.compressed_size = claimed_size;

    tile_chunk.payload = serialize_tile_header(header);
    for (uint32_t i = 0; i < actual_size; ++i) {
        tile_chunk.payload.push_back(static_cast<uint8_t>(i + 1));
    }
    return tile_chunk;
}

void append_chunk(ByteWriter& writer, const char type[4], uint8_t flags, std::span<const uint8_t> payload) {
    writer.write_bytes({reinterpret_cast<const uint8_t*>(type), 4});
    writer.write_u8(flags);
    writer.write_u32(static_cast<uint32_t>(payload.size()));
    writer.write_bytes(payload);
}

FrameHeader make_test_header() {
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

}

TEST(ContainerTest, FhdrRoundTrip) {
    FrameHeader hdr = make_test_header();

    auto bytes = serialize_fhdr(hdr);
    auto parsed = parse_fhdr(bytes);
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->width, 640u);
    EXPECT_EQ(parsed->height, 480u);
    EXPECT_EQ(parsed->bit_depth, 8);
    EXPECT_EQ(parsed->cicp_primaries, 1);
    EXPECT_EQ(parsed->cicp_transfer, 1);
    EXPECT_EQ(parsed->cicp_matrix, 1);
    EXPECT_EQ(parsed->tile_size_log2, 9);
    EXPECT_TRUE(parsed->is_tiled());
    EXPECT_TRUE(parsed->full_range());
    EXPECT_FALSE(parsed->is_lossless());
}

TEST(ContainerTest, HdrHeader) {
    FrameHeader hdr;
    hdr.width = 3840;
    hdr.height = 2160;
    hdr.bit_depth = 10;
    hdr.cicp_primaries = 9;
    hdr.cicp_transfer = 16;
    hdr.cicp_matrix = 9;
    hdr.flags = FHDR_FLAG_HDR | FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    hdr.tile_size_log2 = 9;
    hdr.max_cll = 1000;
    hdr.max_fall = 400;

    auto bytes = serialize_fhdr(hdr);
    auto parsed = parse_fhdr(bytes);
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->bit_depth, 10);
    EXPECT_EQ(parsed->cicp_transfer, 16);
    EXPECT_TRUE(parsed->is_hdr());
    EXPECT_EQ(parsed->max_cll, 1000u);
    EXPECT_EQ(parsed->max_fall, 400u);
}

TEST(ContainerTest, AnimRoundTrip) {
    AnimHeader anim;
    anim.frame_count = 3;
    anim.loop_count = 0;
    anim.background_rgba = 0x00000000;

    for (int i = 0; i < 3; ++i) {
        AnimFrame frame;
        frame.delay_ms = 100;
        frame.blend_mode = 0;
        frame.disposal = 0;
        frame.rect_x = 0;
        frame.rect_y = 0;
        frame.rect_w = 100;
        frame.rect_h = 100;
        frame.tile_offset = static_cast<uint32_t>(i * 1000);
        anim.frames.push_back(frame);
    }

    auto bytes = serialize_anim(anim);
    auto parsed = parse_anim(bytes);
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->frame_count, 3u);
    EXPECT_EQ(parsed->loop_count, 0u);
    EXPECT_EQ(parsed->frames.size(), 3u);
    EXPECT_EQ(parsed->frames[1].delay_ms, 100u);
    EXPECT_EQ(parsed->frames[2].tile_offset, 2000u);
}

TEST(ContainerTest, TileHeaderRoundTrip) {
    TileHeader tile;
    tile.tile_x = 3;
    tile.tile_y = 7;
    tile.layer_flags = TILE_HAS_BASE | TILE_HAS_REFINEMENT;
    tile.compressed_size = 12345;

    auto bytes = serialize_tile_header(tile);
    auto parsed = parse_tile_header(bytes);
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->tile_x, 3u);
    EXPECT_EQ(parsed->tile_y, 7u);
    EXPECT_EQ(parsed->layer_flags, TILE_HAS_BASE | TILE_HAS_REFINEMENT);
    EXPECT_EQ(parsed->compressed_size, 12345u);
}

TEST(ContainerTest, FullContainerRoundTrip) {
    WkFile file;
    file.header = make_test_header();
    file.header.flags |= FHDR_FLAG_HAS_WKMETA;

    meta::MetaBlock meta;
    ASSERT_TRUE(meta.set(meta::Namespace::Geo, meta::geo::LAT, 1.234).has_value());
    file.metadata = std::move(meta);
    file.tile_chunks.push_back(make_tile_chunk(4, 4));

    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());
    EXPECT_GT(written->size(), 0u);
    EXPECT_EQ((*written)[0], 0x57);
    EXPECT_EQ((*written)[1], 0x4B);
    EXPECT_EQ((*written)[2], 0x49);
    EXPECT_EQ((*written)[3], 0x4D);
    EXPECT_EQ((*written)[4], 0x47);

    auto parsed = parse_container(*written);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->header.width, 640u);
    EXPECT_EQ(parsed->header.height, 480u);
    EXPECT_TRUE(parsed->metadata.has_value());
    EXPECT_EQ(parsed->tile_chunks.size(), 1u);
}

TEST(ContainerTest, MissingFendRejected) {
    WkFile file;
    file.header = make_test_header();
    file.tile_chunks.push_back(make_tile_chunk(4, 4));

    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());
    ASSERT_GE(written->size(), 9u);
    written->resize(written->size() - 9u);

    auto parsed = parse_container(*written);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidChunkType);
}

TEST(ContainerTest, InvalidTilePayloadSizeRejected) {
    WkFile file;
    file.header = make_test_header();
    file.tile_chunks.push_back(make_tile_chunk(8, 4));

    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto parsed = parse_container(*written);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidChunkSize);
}

TEST(ContainerTest, UnknownRequiredChunkRejected) {
    const auto fhdr = serialize_fhdr(make_test_header());

    ByteWriter writer;
    writer.write_bytes({WK_MAGIC, 5});
    writer.write_u16(WK_VERSION);
    append_chunk(writer, CHUNK_FHDR, 0, fhdr);
    append_chunk(writer, "ABCD", 0, {});
    append_chunk(writer, CHUNK_FEND, 0, {});

    auto parsed = parse_container(writer.finish());
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidChunkType);
}

TEST(ContainerTest, InvalidMagic) {
    uint8_t bad_data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01};
    auto result = parse_container(bad_data);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidMagic);
}

TEST(ContainerTest, TruncatedInput) {
    uint8_t too_short[] = {0x57, 0x4B};
    auto result = parse_container(too_short);
    EXPECT_FALSE(result.has_value());
}

TEST(ContainerTest, InvalidBitDepth) {
    FrameHeader hdr;
    hdr.width = 100;
    hdr.height = 100;
    hdr.bit_depth = 7;
    hdr.tile_size_log2 = 9;

    auto bytes = serialize_fhdr(hdr);
    bytes[8] = 7;

    auto parsed = parse_fhdr(bytes);
    EXPECT_FALSE(parsed.has_value());
}

TEST(ContainerTest, TileCount) {
    FrameHeader hdr;
    hdr.width = 1920;
    hdr.height = 1080;
    hdr.tile_size_log2 = 9;

    EXPECT_EQ(hdr.tile_size(), 512u);
    EXPECT_EQ(hdr.tiles_x(), 4u);
    EXPECT_EQ(hdr.tiles_y(), 3u);
    EXPECT_EQ(hdr.tile_count(), 12u);
}

// Regression tests for the Critical allocation-safety review (2026-11-13):
// parse_anim used to resize(frame_count) from the file before checking the
// payload size, so a 10-byte ANIM chunk claiming 0xFFFFFFFF frames aborted
// the decoder with std::bad_alloc. parse_fhdr accepted any non-zero
// dimension, so a 27-byte header could trigger a multi-GB Image allocation
// in decode() before any tile was checked.
TEST(ContainerTest, ParseAnimRejectsFrameCountLargerThanPayload) {
    ByteWriter writer;
    writer.write_u32(0xFFFFFFFFu);
    writer.write_u16(0);
    writer.write_u32(0);

    auto parsed = parse_anim(writer.finish());
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::AnimationError);
}

TEST(ContainerTest, ParseAnimRejectsTruncatedFrameEntries) {
    // Claims 2 frames (needs 10 + 2*16 bytes) but carries only 1 frame.
    ByteWriter writer;
    writer.write_u32(2);
    writer.write_u16(0);
    writer.write_u32(0);
    for (int i = 0; i < 16; ++i) {
        writer.write_u8(0);
    }

    auto parsed = parse_anim(writer.finish());
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::AnimationError);
}

TEST(ContainerTest, ParseFhdrRejectsOversizedDimensions) {
    FrameHeader hdr;
    hdr.width = 40000;
    hdr.height = 40000;
    hdr.bit_depth = 8;
    hdr.tile_size_log2 = 9;

    auto parsed = parse_fhdr(serialize_fhdr(hdr));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidHeader);
}

TEST(ContainerTest, ParseFhdrRejectsDimensionAboveMax) {
    FrameHeader hdr;
    hdr.width = 65536;
    hdr.height = 64;
    hdr.bit_depth = 8;
    hdr.tile_size_log2 = 6;

    auto parsed = parse_fhdr(serialize_fhdr(hdr));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidHeader);
}

TEST(ContainerTest, ParseFhdrAcceptsMaxSupportedDimensions) {
    // 16384 x 16384 = 2^28 pixels, exactly at the supported cap.
    FrameHeader hdr;
    hdr.width = 16384;
    hdr.height = 16384;
    hdr.bit_depth = 8;
    hdr.tile_size_log2 = 10;

    auto parsed = parse_fhdr(serialize_fhdr(hdr));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->width, 16384u);
    EXPECT_EQ(parsed->height, 16384u);
}

TEST(ContainerTest, ParseFhdrRejectsJustOverPixelCap) {
    // 16385 x 16384 = 268,451,840 pixels, one row over the 2^28 cap.
    FrameHeader hdr;
    hdr.width = 16385;
    hdr.height = 16384;
    hdr.bit_depth = 8;
    hdr.tile_size_log2 = 10;

    auto parsed = parse_fhdr(serialize_fhdr(hdr));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::InvalidHeader);
}
