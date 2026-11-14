// Phase 3 superblock quant map implementation. See superblock_quant.h.
#include "superblock_quant.h"

#include "quantize.h"

namespace wk {
namespace {

// Quant scale index for identity (1.0). The textured-superblock
// coarser step (scale index 5 = 1.25) is DISABLED by measurement
// (2026-11-14, option (d) policy review) and must not be re-enabled
// by only changing this constant — see aggregate_superblock_scales.
//
// What was measured (6 benchmark photos, q75/q85, V10 default):
//   * threshold 0.75 (the old policy): total -0.48% q75 / -0.44% q85,
//     but meadow PSNR -0.97 dB q75 (40.45 -> 39.48 dB, SSIM -0.002)
//     and -0.61 dB q85. The saving came almost entirely from meadow
//     itself (4,124 of 5,794 B saved at q75), i.e. the policy bought
//     size by hurting exactly the photo it hurt most.
//   * thresholds 0.85 / 0.90 / 1.0 (only 100%-textured superblocks):
//     meadow recovered only to 39.59 / 39.62 / 39.91 dB at q75 —
//     still -0.54 dB at 1.0 — because meadow grass is uniformly
//     textured at the superblock level (19 of 90 superblocks are
//     >=75% textured, mean textured fraction 0.97).
//   * no activity/edge gate separates the cases either: hit
//     superblocks in meadow have mean activity 0.99, in ember and
//     noctis 0.91 — a saturated, razor-thin gap — and edge fraction
//     is ~0 everywhere. The maximum "safe" saving (ember + noctis
//     only) is ~0.14% of total size at q75: not worth any policy.
//   * conclusion: with the current Textured classifier the "busy
//     blocks mask coarser quant" assumption does not hold — the
//     texture here is real detail, not masking-friendly noise, and
//     the luma-prediction investigation the same day found the same
//     thing (residual dominated by unpredictable texture). So the
//     quant bias is off; the control field stays computed (it may
//     guide transform/filter choices later, roadmap Phase 2) and the
//     superblock map format is unchanged (old V9/V10 files with a
//     non-trivial map still decode; the encoder just never emits one
//     from the field now).
constexpr uint8_t kScaleIdentity = 4;

}  // namespace

std::vector<uint8_t> aggregate_superblock_scales(
    const std::vector<BlockControl>& field, uint32_t blocks_x, uint32_t blocks_y) {
    // The textured-superblock coarser step is disabled (see above):
    // every superblock stays at identity regardless of content.
    // `field` is kept in the signature so the encoder's call site and
    // any future policy do not need to change; it is intentionally
    // unused here.
    (void)field;
    const uint32_t sbx_count = superblocks_x(blocks_x);
    const uint32_t sby_count = superblocks_y(blocks_y);
    return std::vector<uint8_t>(sbx_count * sby_count, kScaleIdentity);
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
