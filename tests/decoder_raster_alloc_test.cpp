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

// Decoder must fail fast on a crafted huge-dimension file without
// allocating frame-sized rasters first (#hole8): a 16384x16384 header
// used to cost ~1.6 GB (10-bit output raster) before the tile-count
// check rejected the file. These tests assert a clean error AND a
// small peak RSS, so the fix is measured, not assumed.

#include <gtest/gtest.h>
#include "../src/core/container.h"
#include <wk/wk.hpp>
#include <sys/resource.h>
#include <cstring>
#include <iostream>

using namespace wk;

namespace {

// Peak RSS of this process in KiB (Linux ru_maxrss is a high-water
// mark; this binary only runs these tests, so the reading after
// decode is the decode's own peak).
long peak_rss_kib() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

// A crafted file may not peak above this: the input files below are
// a few KiB, so anything near it means a frame-sized raster was
// allocated. 512 MiB is far above the fixed behaviour (~tens of MB)
// and far below the old behaviour (0.8-2.6 GB).
constexpr long kMaxPeakRssKib = 512L * 1024;

FrameHeader make_huge_header(bool lossless, uint8_t bit_depth) {
    FrameHeader hdr;
    hdr.width = 16384;
    hdr.height = 16384;
    hdr.bit_depth = bit_depth;
    hdr.cicp_primaries = 1;
    hdr.cicp_transfer = 1;
    hdr.cicp_matrix = 1;
    hdr.flags = FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    if (lossless) hdr.flags |= FHDR_FLAG_LOSSLESS;
    // 10-bit frames must carry the HDR flag (parse_fhdr enforces
    // hdr_flag_expected): without it the file fails at parse time,
    // before any allocation, and the test would not exercise decode.
    if (bit_depth > 8) hdr.flags |= FHDR_FLAG_HDR;
    hdr.tile_size_log2 = 10;  // 1024px tiles -> 16x16 grid
    return hdr;
}

void append_chunk(ByteWriter& w, const char type[4], uint8_t flags,
                  std::span<const uint8_t> payload) {
    w.write_bytes({reinterpret_cast<const uint8_t*>(type), 4});
    w.write_u8(flags);
    w.write_u32(static_cast<uint32_t>(payload.size()));
    w.write_bytes(payload);
}

Chunk make_tile_chunk(uint16_t tx, uint16_t ty, uint32_t payload_bytes) {
    Chunk tile;
    std::memcpy(tile.type, CHUNK_TILE, 4);
    TileHeader header;
    header.tile_x = tx;
    header.tile_y = ty;
    header.layer_flags = TILE_HAS_BASE;
    header.compressed_size = payload_bytes;
    tile.payload = serialize_tile_header(header);
    tile.payload.insert(tile.payload.end(), payload_bytes, 0);
    return tile;
}

std::vector<uint8_t> build_file(const FrameHeader& hdr,
                                const std::vector<Chunk>& tiles) {
    ByteWriter w;
    w.write_bytes({WK_MAGIC, 5});
    w.write_u16(WK_VERSION);
    append_chunk(w, CHUNK_FHDR, 0, serialize_fhdr(hdr));
    for (const auto& tile : tiles) {
        append_chunk(w, CHUNK_TILE, CHUNK_FLAG_REPEATABLE, tile.payload);
    }
    append_chunk(w, CHUNK_FEND, 0, {});
    return w.finish();
}

}  // namespace

TEST(DecoderRasterAlloc, LossyWrongTileCountFailsSmall) {
    // 10-bit 16384x16384, grid needs 256 tiles, file carries 1.
    auto file = build_file(make_huge_header(false, 10),
                           {make_tile_chunk(0, 0, 16)});
    auto result = decode(file);
    EXPECT_FALSE(result.has_value()) << "wrong tile count must fail";
    const long peak = peak_rss_kib();
    std::cout << "LossyWrongTileCount peak RSS: " << peak / 1024 << " MiB\n";
    EXPECT_LT(peak, kMaxPeakRssKib) << "decoder allocated a frame raster before rejecting";
}

TEST(DecoderRasterAlloc, LossyCorrectCountTruncatedPayloadsFailsSmall) {
    // Grid count matches (256 tiles) but every payload is 16 bytes:
    // far below the 268-byte lossy tile minimum (quant tables + block
    // dims + layout tag), so the file is provably truncated.
    std::vector<Chunk> tiles;
    for (uint16_t y = 0; y < 16; ++y)
        for (uint16_t x = 0; x < 16; ++x)
            tiles.push_back(make_tile_chunk(x, y, 16));
    auto file = build_file(make_huge_header(false, 10), tiles);
    auto result = decode(file);
    EXPECT_FALSE(result.has_value()) << "truncated tile payloads must fail";
    const long peak = peak_rss_kib();
    std::cout << "LossyCorrectCountTruncated peak RSS: " << peak / 1024 << " MiB\n";
    EXPECT_LT(peak, kMaxPeakRssKib) << "decoder allocated frame planes before validating payloads";
}

TEST(DecoderRasterAlloc, LosslessSingleTileTruncatedFailsSmall) {
    // Lossless 16384x16384, one tile, 2-byte payload (flags + predictor
    // only): no rANS stream can follow, so the decode must fail before
    // any frame-sized allocation.
    Chunk tile;
    std::memcpy(tile.type, CHUNK_TILE, 4);
    TileHeader header;
    header.tile_x = 0;
    header.tile_y = 0;
    header.layer_flags = TILE_HAS_BASE;
    header.compressed_size = 2;
    tile.payload = serialize_tile_header(header);
    tile.payload.push_back(0);  // flags: predictor path, no palette
    tile.payload.push_back(0);  // predictor mode 0
    auto file = build_file(make_huge_header(true, 8), {tile});
    auto result = decode(file);
    EXPECT_FALSE(result.has_value()) << "truncated lossless payload must fail";
    const long peak = peak_rss_kib();
    std::cout << "LosslessSingleTileTruncated peak RSS: " << peak / 1024 << " MiB\n";
    EXPECT_LT(peak, kMaxPeakRssKib) << "decoder allocated a frame raster before validating the payload";
}

TEST(DecoderRasterAlloc, SmallValidFilesStillDecode) {
    // Positive control: the restructured paths must still decode real
    // files (lossy + lossless) bit-identically to the old paths.
    Image gradient(64, 64, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
            uint8_t* p = gradient.row(y) + x * 3;
            p[0] = static_cast<uint8_t>(x * 4);
            p[1] = static_cast<uint8_t>(y * 4);
            p[2] = 128;
        }
    EncoderConfig lossy;
    lossy.quality = 75;
    auto encoded = encode(gradient, lossy);
    ASSERT_TRUE(encoded.has_value());
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width(), 64u);
    EXPECT_EQ(decoded->height(), 64u);

    EncoderConfig lossless;
    lossless.lossless = true;
    auto enc_ll = encode(gradient, lossless);
    ASSERT_TRUE(enc_ll.has_value());
    auto dec_ll = decode(*enc_ll);
    ASSERT_TRUE(dec_ll.has_value());
    EXPECT_EQ(dec_ll->pixels().size(), gradient.pixels().size());
    EXPECT_EQ(std::memcmp(dec_ll->pixels().data(), gradient.pixels().data(),
                          gradient.pixels().size()),
              0);
}
