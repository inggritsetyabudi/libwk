// Phase 2 perceptual control field: per-block content analysis that
// guides the encoder's local decisions (quant bias first; transform,
// tool and filter choices later). The field is computed from the luma
// plane only — luma drives perceived quality most — and is deliberately
// simple and robust: a block is Flat (smooth, banding-prone), Textured
// (busy, masking hides errors), Edge (strong structure), or Normal.
//
// The field's quant suggestion is threaded into the existing V7
// per-block quant-scale mechanism (encoder.cpp); it does not change
// the bitstream format. The scale mapping is deliberately one-sided:
// only textured blocks go coarser (1.25), where masking hides the
// loss. Finer quant on flat blocks was tried and REJECTED by
// measurement (2026-11-14): it grew files +24% to +39% while PSNR
// fell, because finer quant is far more expensive in bits than the
// coarser quant on textured blocks saves. The seed boost already
// covers the worst banding case; flat blocks stay at identity.
#pragma once

#include <cstdint>
#include <vector>

namespace wk {

// Per-block content classification for the control field.
enum class BlockContent : uint8_t {
    Flat,      // smooth, low variance — banding-prone, finer quant helps
    Textured,  // busy, high variance — masking hides errors, coarser is fine
    Edge,      // strong structure — preserve sharpness, keep identity
    Normal,    // everything else — identity
};

struct BlockControl {
    BlockContent content = BlockContent::Normal;
    float activity = 0.0f;       // normalized texture/activity, 0..1
    float edge_strength = 0.0f;  // normalized edge strength, 0..1
    // Quant scale index into QuantTable::kQuantScaleFactors (4 = identity).
    // Set by the field; the encoder may override it (e.g. seed boost).
    uint8_t quant_scale_index = 4;
};

// Analyzes one tile's luma plane and returns a per-block control field
// in raster order. tile_luma is the tile's luma pixels (tw x th,
// row-major, int16_t); blocks_per_row = ceil(tw/8),
// blocks_per_col = ceil(th/8). Blocks on the tile edge are padded
// with their edge pixel (the encoder already pads the tile the same
// way), so a partial block is analyzed on its real pixels only.
std::vector<BlockControl> analyze_control_field(
    const int16_t* tile_luma, uint32_t tw, uint32_t th);

}  // namespace wk
