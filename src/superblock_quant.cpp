// Phase 3 superblock quant map implementation. See superblock_quant.h.
#include "superblock_quant.h"

#include "quantize.h"

namespace wk {
namespace {

// Quant scale indices into QuantTable::kQuantScaleFactors.
constexpr uint8_t kScaleIdentity = 4;
constexpr uint8_t kScaleCoarser = 5;  // 1.25 — textured superblocks

// A superblock goes coarser when at least this fraction of its blocks
// are Textured. 0.75 is conservative: the superblock must be clearly
// dominated by texture for the coarser step to be safe. 0.5 was tried
// and rejected (2026-11-14): meadow (grass detail) lost 1.27 dB PSNR
// at q75 for only 3.6% saving, because its texture is real detail,
// not masking-friendly noise — a majority-textured superblock is not
// textured enough.
constexpr float kTexturedFractionMin = 0.75f;

}  // namespace

std::vector<uint8_t> aggregate_superblock_scales(
    const std::vector<BlockControl>& field, uint32_t blocks_x, uint32_t blocks_y) {
    const uint32_t sbx_count = superblocks_x(blocks_x);
    const uint32_t sby_count = superblocks_y(blocks_y);
    std::vector<uint8_t> scales(sbx_count * sby_count, kScaleIdentity);
    for (uint32_t sby = 0; sby < sby_count; ++sby) {
        for (uint32_t sbx = 0; sbx < sbx_count; ++sbx) {
            size_t textured = 0;
            size_t total = 0;
            for (uint32_t by = sby * kSuperblockBlocks;
                 by < std::min((sby + 1) * kSuperblockBlocks, blocks_y); ++by) {
                for (uint32_t bx = sbx * kSuperblockBlocks;
                     bx < std::min((sbx + 1) * kSuperblockBlocks, blocks_x); ++bx) {
                    const size_t idx = static_cast<size_t>(by) * blocks_x + bx;
                    if (idx >= field.size()) continue;
                    ++total;
                    if (field[idx].content == BlockContent::Textured) ++textured;
                }
            }
            if (total > 0 &&
                static_cast<float>(textured) / static_cast<float>(total) >= kTexturedFractionMin) {
                scales[sby * sbx_count + sbx] = kScaleCoarser;
            }
        }
    }
    return scales;
}

bool superblock_map_is_nontrivial(const std::vector<uint8_t>& scales) {
    for (uint8_t s : scales) {
        if (s != kScaleIdentity) return true;
    }
    return false;
}

uint8_t superblock_scale_for(const std::vector<uint8_t>& scales,
                             uint32_t blocks_x, uint32_t bx, uint32_t by) {
    if (scales.empty()) return kScaleIdentity;
    const uint32_t sbx = bx / kSuperblockBlocks;
    const uint32_t sby = by / kSuperblockBlocks;
    const uint32_t sbx_count = superblocks_x(blocks_x);
    const size_t idx = static_cast<size_t>(sby) * sbx_count + sbx;
    if (idx >= scales.size()) return kScaleIdentity;
    return scales[idx];
}

Result<void> write_superblock_quant_map(
    ByteWriter& writer, const std::vector<uint8_t>& scales) {
    uint16_t count = 0;
    for (uint8_t s : scales) {
        if (s != kScaleIdentity) ++count;
    }
    writer.write_u16(count);
    for (size_t i = 0; i < scales.size(); ++i) {
        if (scales[i] != kScaleIdentity) {
            writer.write_u16(static_cast<uint16_t>(i));
            writer.write_u8(scales[i]);
        }
    }
    return {};
}

Result<std::vector<uint8_t>> read_superblock_quant_map(
    ByteReader& reader, size_t superblock_count) {
    std::vector<uint8_t> scales(superblock_count, kScaleIdentity);
    auto count = reader.read_u16();
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count > superblock_count) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
            "superblock quant map count exceeds superblock count"});
    }
    size_t prev_index = 0;
    bool first = true;
    for (uint16_t i = 0; i < *count; ++i) {
        auto index = reader.read_u16();
        if (!index) {
            return std::unexpected(index.error());
        }
        auto scale = reader.read_u8();
        if (!scale) {
            return std::unexpected(scale.error());
        }
        if (*index >= superblock_count) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                "superblock quant map index out of range"});
        }
        if (!first && *index <= prev_index) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                "superblock quant map indices not strictly increasing"});
        }
        if (*scale >= QuantTable::kQuantScaleFactors.size()) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                "superblock quant map scale index out of range"});
        }
        scales[*index] = *scale;
        prev_index = *index;
        first = false;
    }
    return scales;
}

}  // namespace wk
