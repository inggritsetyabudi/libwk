// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "../src/common.h"
#include "../src/container.h"
#include "../src/tile_util.h"

using namespace wk;

namespace {

// Builds a TILE chunk payload: 9-byte tile header + `payload_bytes` of
// dummy data. The tile header's compressed_size is set to the dummy
// payload size unless `claimed_size` overrides it.
std::vector<uint8_t> MakeTilePayload(uint16_t tile_x, uint16_t tile_y,
                                     uint8_t layer_flags, size_t payload_bytes,
                                     uint32_t claimed_size = 0xFFFFFFFFu) {
    std::vector<uint8_t> payload(9 + payload_bytes, 0);
    payload[0] = static_cast<uint8_t>(tile_x & 0xFF);
    payload[1] = static_cast<uint8_t>(tile_x >> 8);
    payload[2] = static_cast<uint8_t>(tile_y & 0xFF);
    payload[3] = static_cast<uint8_t>(tile_y >> 8);
    payload[4] = layer_flags;
    const uint32_t size = claimed_size == 0xFFFFFFFFu
                              ? static_cast<uint32_t>(payload_bytes)
                              : claimed_size;
    payload[5] = static_cast<uint8_t>(size & 0xFF);
    payload[6] = static_cast<uint8_t>((size >> 8) & 0xFF);
    payload[7] = static_cast<uint8_t>((size >> 16) & 0xFF);
    payload[8] = static_cast<uint8_t>((size >> 24) & 0xFF);
    return payload;
}

Chunk MakeTileChunk(uint16_t tile_x, uint16_t tile_y, uint8_t layer_flags,
                    size_t payload_bytes, uint32_t claimed_size = 0xFFFFFFFFu) {
    Chunk chunk;
    std::memcpy(chunk.type, CHUNK_TILE, 4);
    chunk.flags = CHUNK_FLAG_REPEATABLE;
    chunk.payload = MakeTilePayload(tile_x, tile_y, layer_flags, payload_bytes, claimed_size);
    return chunk;
}

}  // namespace

TEST(TileUtilTest, PayloadViewValid) {
    auto chunk = MakeTileChunk(1, 2, TILE_HAS_BASE, 100);
    TileHeader header;
    auto view = tile_payload_view(chunk, header);
    ASSERT_TRUE(view.has_value()) << view.error().message;
    EXPECT_EQ(header.tile_x, 1u);
    EXPECT_EQ(header.tile_y, 2u);
    EXPECT_EQ(header.layer_flags, TILE_HAS_BASE);
    EXPECT_EQ(header.compressed_size, 100u);
    EXPECT_EQ(view->size(), 100u);
}

TEST(TileUtilTest, PayloadViewTooShort) {
    Chunk chunk;
    std::memcpy(chunk.type, CHUNK_TILE, 4);
    chunk.payload = {0, 0, 0, 0, 0, 0, 0, 0};  // 8 bytes < 9-byte header
    TileHeader header;
    auto view = tile_payload_view(chunk, header);
    EXPECT_FALSE(view.has_value());
    EXPECT_EQ(view.error().code, ErrorCode::InvalidChunkSize);
}

TEST(TileUtilTest, PayloadViewSizeMismatch) {
    auto chunk = MakeTileChunk(0, 0, TILE_HAS_BASE, 100, /*claimed_size=*/50);
    TileHeader header;
    auto view = tile_payload_view(chunk, header);
    EXPECT_FALSE(view.has_value());
    EXPECT_EQ(view.error().code, ErrorCode::InvalidChunkSize);
}

TEST(TileUtilTest, Geometry444) {
    auto geometry = infer_tile_geometry(0, 0, 512, 1024, 1024,
                                        /*blocks=*/64, 64,
                                        /*chroma_blocks=*/64, 64);
    ASSERT_TRUE(geometry.has_value()) << geometry.error().message;
    EXPECT_EQ(geometry->x0, 0u);
    EXPECT_EQ(geometry->y0, 0u);
    EXPECT_EQ(geometry->tw, 512u);
    EXPECT_EQ(geometry->th, 512u);
    EXPECT_EQ(geometry->subsampling, ChromaSubsampling::YUV444);
    EXPECT_EQ(geometry->chroma_tw, 512u);
    EXPECT_EQ(geometry->chroma_th, 512u);
}

TEST(TileUtilTest, Geometry420) {
    auto geometry = infer_tile_geometry(0, 0, 512, 1024, 1024,
                                        /*blocks=*/64, 64,
                                        /*chroma_blocks=*/32, 32);
    ASSERT_TRUE(geometry.has_value()) << geometry.error().message;
    EXPECT_EQ(geometry->subsampling, ChromaSubsampling::YUV420);
    EXPECT_EQ(geometry->chroma_tw, 256u);
    EXPECT_EQ(geometry->chroma_th, 256u);
}

TEST(TileUtilTest, GeometryEdgeTile) {
    // 100x100 image, 64px tiles: tile (1,1) covers the 36x36 corner.
    auto geometry = infer_tile_geometry(1, 1, 64, 100, 100,
                                        /*blocks=*/5, 5,
                                        /*chroma_blocks=*/5, 5);
    ASSERT_TRUE(geometry.has_value()) << geometry.error().message;
    EXPECT_EQ(geometry->x0, 64u);
    EXPECT_EQ(geometry->y0, 64u);
    EXPECT_EQ(geometry->tw, 36u);
    EXPECT_EQ(geometry->th, 36u);
}

TEST(TileUtilTest, GeometryOutOfRange) {
    // Tile (2,0) starts at x=128, past the 100px-wide image.
    auto geometry = infer_tile_geometry(2, 0, 64, 100, 100,
                                        /*blocks=*/1, 1,
                                        /*chroma_blocks=*/1, 1);
    EXPECT_FALSE(geometry.has_value());
    EXPECT_EQ(geometry.error().code, ErrorCode::InvalidTileCoord);
}

TEST(TileUtilTest, GeometryWrongLumaBlockDims) {
    auto geometry = infer_tile_geometry(0, 0, 512, 1024, 1024,
                                        /*blocks=*/63, 64,
                                        /*chroma_blocks=*/64, 64);
    EXPECT_FALSE(geometry.has_value());
    EXPECT_EQ(geometry.error().code, ErrorCode::DecodeFailed);
}

TEST(TileUtilTest, GeometryWrongChromaBlockDims) {
    // Chroma 33x32 matches neither 444 (64x64) nor 420 (32x32).
    auto geometry = infer_tile_geometry(0, 0, 512, 1024, 1024,
                                        /*blocks=*/64, 64,
                                        /*chroma_blocks=*/33, 32);
    EXPECT_FALSE(geometry.has_value());
    EXPECT_EQ(geometry.error().code, ErrorCode::DecodeFailed);
}

TEST(TileUtilTest, GeometryAmbiguous8x8Tile) {
    // An 8x8 tile has 1x1 blocks in both 444 and 420, so the counts
    // coincide; the preferred subsampling decides, defaulting to 444.
    auto no_pref = infer_tile_geometry(0, 0, 8, 8, 8,
                                       /*blocks=*/1, 1,
                                       /*chroma_blocks=*/1, 1);
    ASSERT_TRUE(no_pref.has_value()) << no_pref.error().message;
    EXPECT_EQ(no_pref->subsampling, ChromaSubsampling::YUV444);

    auto pref_420 = infer_tile_geometry(0, 0, 8, 8, 8,
                                        /*blocks=*/1, 1,
                                        /*chroma_blocks=*/1, 1,
                                        ChromaSubsampling::YUV420);
    ASSERT_TRUE(pref_420.has_value()) << pref_420.error().message;
    EXPECT_EQ(pref_420->subsampling, ChromaSubsampling::YUV420);
    EXPECT_EQ(pref_420->chroma_tw, 4u);
    EXPECT_EQ(pref_420->chroma_th, 4u);

    auto pref_444 = infer_tile_geometry(0, 0, 8, 8, 8,
                                        /*blocks=*/1, 1,
                                        /*chroma_blocks=*/1, 1,
                                        ChromaSubsampling::YUV444);
    ASSERT_TRUE(pref_444.has_value()) << pref_444.error().message;
    EXPECT_EQ(pref_444->subsampling, ChromaSubsampling::YUV444);
}

TEST(TileUtilTest, InferSubsamplingFromTile420) {
    // Tile payload: 256 bytes of quant tables + 4 u16 block dims.
    std::vector<uint8_t> tile_data(256 + 8, 0);
    auto put_u16 = [&](size_t off, uint16_t v) {
        tile_data[off] = static_cast<uint8_t>(v & 0xFF);
        tile_data[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    // 512px tile on a 1024x1024 image: 64x64 luma blocks, 32x32 chroma.
    put_u16(256, 64);
    put_u16(258, 64);
    put_u16(260, 32);
    put_u16(262, 32);

    auto subsampling = infer_subsampling_from_tile(tile_data, 0, 0, 512, 1024, 1024);
    ASSERT_TRUE(subsampling.has_value()) << subsampling.error().message;
    EXPECT_EQ(*subsampling, ChromaSubsampling::YUV420);
}

TEST(TileUtilTest, InferSubsamplingFromTileTruncated) {
    std::vector<uint8_t> tile_data(100, 0);  // less than 256-byte quant tables
    auto subsampling = infer_subsampling_from_tile(tile_data, 0, 0, 512, 1024, 1024);
    EXPECT_FALSE(subsampling.has_value());
}
