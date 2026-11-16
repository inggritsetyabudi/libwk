// Phase 3 superblock quant map: the control field's per-block content
// is aggregated to one quant scale per superblock (64x64 pixels), which
// is cheap to signal (far fewer entries than per-block) and is what
// makes the Phase 2 control field usable. The Phase 2 per-block V7
// scales were rejected by measurement: the sparse per-block list costs
// 3 B per non-identity block, and a dense field (e.g. noctis 83%
// textured) made the scale stream alone cost ~17 KB, eating the
// coefficient saving. A superblock map has 64x fewer entries, so the
// signaling is negligible.
//
// The map is shared by luma and chroma (one scale per superblock);
// chroma follows luma content, which is the perceptually right
// behavior. The textured-superblock coarser step (1.25) is DISABLED
// by measurement (2026-11-14, option (d) policy review): threshold
// tuning (0.75 / 0.85 / 0.90 / 1.0) could not recover meadow's
// -0.97 dB PSNR loss, because the texture is real detail, not
// masking-friendly noise. Every superblock currently stays at
// identity; the map format is unchanged, so old V9/V10 files with a
// non-trivial map still decode. There is deliberately no finer
// scale: finer quant on smooth blocks was measured and rejected
// (Phase 2 variant 1 grew files +24% to +39%).
#pragma once

#include <cstdint>
#include <vector>

#include "common.h"
#include "control_field.h"

namespace wk {

// A superblock is 64x64 pixels = 8x8 blocks of 8x8.
inline constexpr uint32_t kSuperblockPixels = 64;
inline constexpr uint32_t kSuperblockBlocks = 8;

// Number of superblocks across / down a block grid.
inline uint32_t superblocks_x(uint32_t blocks_x) { return (blocks_x + 7) / 8; }
inline uint32_t superblocks_y(uint32_t blocks_y) { return (blocks_y + 7) / 8; }

// Aggregates a per-block control field into one quant scale index per
// superblock (raster order, superblocks_x * superblocks_y entries).
// Currently every superblock stays at identity (index 4 = 1.0): the
// textured-superblock coarser step is disabled by measurement (see the
// file comment above and src/superblock_quant.cpp). The `field`
// parameter is kept so the call site and any future policy do not
// need to change.
// `blocks_x` is the tile's block-grid width (the field is
// blocks_x * blocks_y entries, raster order).
std::vector<uint8_t> aggregate_superblock_scales(
    const std::vector<BlockControl>& field, uint32_t blocks_x, uint32_t blocks_y);

// True when the map has at least one non-identity entry (i.e. the
// tile is worth writing as V9).
bool superblock_map_is_nontrivial(const std::vector<uint8_t>& scales);

// Quant scale index for the superblock containing block (bx, by).
// `scales` is the aggregated map; `blocks_x` is the block-grid width.
uint8_t superblock_scale_for(const std::vector<uint8_t>& scales,
                             uint32_t blocks_x, uint32_t bx, uint32_t by);

// Writes the superblock quant map as a sparse list: `count: uint16`,
// then `count` entries of `(superblock_index: uint16, scale_index:
// uint8)` in raster order, only for non-identity superblocks. A
// superblock not listed uses scale 1.0. Returns false (as an
// unexpected Error) only on a writer failure; the map itself is
// always well-formed by construction.
[[nodiscard]] Result<void> write_superblock_quant_map(
    ByteWriter& writer, const std::vector<uint8_t>& scales);

// Reads a superblock quant map written by write_superblock_quant_map
// and returns the dense scale-index vector (identity for unlisted
// superblocks). Strictly validated: count within the superblock
// count, indices in range and strictly increasing, scale index <= 7.
[[nodiscard]] Result<std::vector<uint8_t>> read_superblock_quant_map(
    ByteReader& reader, size_t superblock_count);

}  // namespace wk
