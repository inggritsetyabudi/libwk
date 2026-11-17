// Phase 2 perceptual control field: per-block content analysis that
// guides the encoder's local decisions (quant bias first; transform,
// tool and filter choices later). The field has two layers: the luma
// classes (Flat / Textured / Edge / Normal, from luma variance and
// gradient only) and — when chroma planes are supplied — content-type
// classes (Skin / Sky / Foliage / TextUI / FlatGraphics, from
// full-range BT.709 YCbCr), with the luma classes as fallback. The
// field's quant_scale_index is always derived from the luma class;
// the content classes are informational inside the field. The encoder
// may act on them — the skin policy spends quant scale 0.5 on Skin
// blocks only, luma-only — but that decision lives in the encoder,
// not here.
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
    // Content-type classes, recognized from chroma when available.
    Skin,         // skin tones: Cb/Cr box + hue + chroma variation
    Sky,          // bright blue smooth sky
    Foliage,      // green saturated texture
    TextUI,       // concentrated edge on flat chroma (text/UI)
    FlatGraphics, // smooth vivid flat colour
};

struct BlockControl {
    BlockContent content = BlockContent::Normal;
    float activity = 0.0f;       // normalized texture/activity, 0..1
    float edge_strength = 0.0f;  // normalized edge strength, 0..1
    // Mean chroma gradient over the block's adjacent pixel pairs
    // (mean of |dCb|+|dCr| / 2, same pairs as the luma gradient).
    // Only measured when chroma planes are supplied; 0 otherwise.
    // Real skin is chroma-smooth (p50 0.91); rock/reflection is not
    // (p50 2.50/1.70) — see the skin false-positive research.
    float chroma_grad = 0.0f;
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
//
// When tile_cb and tile_cr are supplied (same layout and dimensions
// as the luma plane, full-range BT.709), blocks are additionally
// classified by content type — Skin, Sky, Foliage, TextUI,
// FlatGraphics — in that priority order, with the luma class as
// fallback for blocks no colour rule claims. When both are null the
// result is exactly the luma-only field.
//
// bit_depth rescales the 8-bit-calibrated thresholds above 8-bit
// (gradients/colours x4, variances x16 at 10-bit; measured slice 0,
// 2026-11-17: 100.00% agreement with the 8-bit classification on
// 176,697 blocks). At 8-bit the behaviour is identical.
std::vector<BlockControl> analyze_control_field(
    const int16_t* tile_luma, uint32_t tw, uint32_t th,
    const int16_t* tile_cb = nullptr, const int16_t* tile_cr = nullptr,
    uint8_t bit_depth = 8);

// Skin false-positive mask (#skin-fp) over a whole-frame field:
// one byte per block (raster order, blocks_per_row * blocks_per_col),
// 1 = drop the skin treatment for that Skin block. A Skin block is
// dropped when it belongs to a wide, flat Skin component (a lake
// reflection or sunlit peak, not a face) or its chroma gradient is
// high (rock, not skin). Non-Skin blocks are always 0. The rule is
// measured, not guessed: (component width >= 40 AND aspect >= 2.0)
// OR chroma_grad >= 3.0 cuts 46.6% of the SKIN_A file cost at ~zero
// GT loss (2026-11-16). The chroma-grad threshold scales with bit
// depth like the field's (x4 at 10-bit). Components are 8-connected
// over the Skin mask of the whole frame — a single tile cannot see
// them, so the caller classifies the frame once and hands the mask
// to the tiles.
std::vector<uint8_t> skin_false_positive_mask(
    const std::vector<BlockControl>& field, uint32_t blocks_per_row,
    uint32_t blocks_per_col, uint8_t bit_depth = 8);

}  // namespace wk
