// Tests for the Phase 3 superblock quant map (superblock_quant.h).
// TDD: the aggregation and the map roundtrip are specified here before
// the encoder/decoder integration is trusted.
#include <gtest/gtest.h>

#include "../src/control_field.h"
#include "../src/superblock_quant.h"

using namespace wk;

namespace {

// A field where every block is Textured (checkerboard-like).
std::vector<BlockControl> all_textured_field(uint32_t blocks_x, uint32_t blocks_y) {
    std::vector<BlockControl> field(blocks_x * blocks_y);
    for (auto& bc : field) {
        bc.content = BlockContent::Textured;
        bc.quant_scale_index = 5;
    }
    return field;
}

// A field where every block is Flat.
std::vector<BlockControl> all_flat_field(uint32_t blocks_x, uint32_t blocks_y) {
    std::vector<BlockControl> field(blocks_x * blocks_y);
    for (auto& bc : field) {
        bc.content = BlockContent::Flat;
        bc.quant_scale_index = 4;
    }
    return field;
}

}  // namespace

TEST(SuperblockQuantTest, AllTexturedTileStaysIdentity) {
    // 16x16 blocks = 2x2 superblocks; every block textured, but the
    // textured-superblock coarser step is DISABLED by measurement
    // (2026-11-14, option (d)): coarsening real texture bought size
    // with disproportionate PSNR loss (meadow -0.97 dB at q75), and
    // no threshold separated safe from harmful superblocks. So every
    // superblock stays at identity and the map is trivial.
    auto field = all_textured_field(16, 16);
    auto scales = aggregate_superblock_scales(field, 16, 16);
    ASSERT_EQ(scales.size(), 4u);
    for (uint8_t s : scales) EXPECT_EQ(s, 4u);
    EXPECT_FALSE(superblock_map_is_nontrivial(scales));
}

TEST(SuperblockQuantTest, AllFlatTileIsAllIdentity) {
    // No textured blocks, so no superblock is non-identity and the
    // tile is not worth writing as V9.
    auto field = all_flat_field(16, 16);
    auto scales = aggregate_superblock_scales(field, 16, 16);
    ASSERT_EQ(scales.size(), 4u);
    for (uint8_t s : scales) EXPECT_EQ(s, 4u);
    EXPECT_FALSE(superblock_map_is_nontrivial(scales));
}

TEST(SuperblockQuantTest, MajorityTexturedSuperblockStaysIdentity) {
    // 8x8 blocks = 1 superblock. 48 of 64 blocks textured (exactly
    // 3/4) used to go coarser under the old 0.75 threshold; the
    // coarser step is now disabled (see AllTexturedTileStaysIdentity),
    // so the superblock stays at identity at 48 and at 47 textured.
    std::vector<BlockControl> field(64);
    for (size_t i = 0; i < 48; ++i) field[i].content = BlockContent::Textured;
    auto scales = aggregate_superblock_scales(field, 8, 8);
    ASSERT_EQ(scales.size(), 1u);
    EXPECT_EQ(scales[0], 4u);

    field[0].content = BlockContent::Flat;  // now 47 textured
    scales = aggregate_superblock_scales(field, 8, 8);
    ASSERT_EQ(scales.size(), 1u);
    EXPECT_EQ(scales[0], 4u);
}

TEST(SuperblockQuantTest, PartialEdgeSuperblockStaysIdentity) {
    // 10x10 blocks: superblocks are 2x2 (ceil), the right/bottom
    // ones are partial (2 wide / 2 tall blocks). The top-right
    // superblock has all 16 of its real blocks textured, which used
    // to go coarser; with the coarser step disabled (see
    // AllTexturedTileStaysIdentity) every superblock stays at
    // identity, and the aggregation still returns the right 2x2 grid.
    std::vector<BlockControl> field(100);
    // Top-right superblock: blocks (8..9, 0..7) -> indices by*10+bx.
    for (uint32_t by = 0; by < 8; ++by) {
        for (uint32_t bx = 8; bx < 10; ++bx) {
            field[by * 10 + bx].content = BlockContent::Textured;
        }
    }
    auto scales = aggregate_superblock_scales(field, 10, 10);
    ASSERT_EQ(scales.size(), 4u);
    EXPECT_EQ(scales[1], 4u);  // top-right, all 16 real blocks textured
    EXPECT_EQ(scales[0], 4u);  // top-left, all flat
    EXPECT_EQ(scales[2], 4u);  // bottom-left, all flat
    EXPECT_EQ(scales[3], 4u);  // bottom-right, all flat
}

TEST(SuperblockQuantTest, MapRoundtrip) {
    // A 4x3 superblock map with a few non-identity entries.
    std::vector<uint8_t> scales = {4, 5, 4, 4, 5, 4, 4, 4, 5, 4, 4, 4};
    ByteWriter writer;
    ASSERT_TRUE(write_superblock_quant_map(writer, scales));
    const auto bytes = writer.finish();
    ByteReader reader(bytes);
    auto decoded = read_superblock_quant_map(reader, scales.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, scales);
}

TEST(SuperblockQuantTest, EmptyMapRoundtrips) {
    std::vector<uint8_t> scales(6, 4);
    ByteWriter writer;
    ASSERT_TRUE(write_superblock_quant_map(writer, scales));
    const auto bytes = writer.finish();
    ByteReader reader(bytes);
    auto decoded = read_superblock_quant_map(reader, scales.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, scales);
}

TEST(SuperblockQuantTest, MapRejectsOutOfRangeIndex) {
    // Hand-craft a map whose entry index exceeds the superblock count.
    ByteWriter writer;
    writer.write_u16(1);
    writer.write_u16(9);  // only 4 superblocks
    writer.write_u8(5);
    const auto bytes = writer.finish();
    ByteReader reader(bytes);
    auto decoded = read_superblock_quant_map(reader, 4);
    EXPECT_FALSE(decoded.has_value());
}

TEST(SuperblockQuantTest, MapRejectsNonIncreasingIndices) {
    ByteWriter writer;
    writer.write_u16(2);
    writer.write_u16(2);
    writer.write_u8(5);
    writer.write_u16(2);  // equal to previous, not strictly increasing
    writer.write_u8(5);
    const auto bytes = writer.finish();
    ByteReader reader(bytes);
    auto decoded = read_superblock_quant_map(reader, 4);
    EXPECT_FALSE(decoded.has_value());
}

TEST(SuperblockQuantTest, MapRejectsBadScaleIndex) {
    ByteWriter writer;
    writer.write_u16(1);
    writer.write_u16(1);
    writer.write_u8(8);  // scale indices are 0..7
    const auto bytes = writer.finish();
    ByteReader reader(bytes);
    auto decoded = read_superblock_quant_map(reader, 4);
    EXPECT_FALSE(decoded.has_value());
}

TEST(SuperblockQuantTest, SuperblockScaleForBlock) {
    // 16x16 blocks = 2x2 superblocks; superblock (1,0) is coarser.
    std::vector<uint8_t> scales = {4, 5, 4, 4};
    EXPECT_EQ(superblock_scale_for(scales, 16, 0, 0), 4u);
    EXPECT_EQ(superblock_scale_for(scales, 16, 7, 7), 4u);
    EXPECT_EQ(superblock_scale_for(scales, 16, 8, 0), 5u);  // sb (1,0)
    EXPECT_EQ(superblock_scale_for(scales, 16, 15, 7), 5u);
    EXPECT_EQ(superblock_scale_for(scales, 16, 8, 8), 4u);  // sb (1,1)
}
