// Cross-component linear model (CCLM) chroma prediction.
//
// The chroma residual correlates strongly with the luma residual
// (measured 2026-11-14: per-pixel correlation up to 0.87 for Cb on
// people photos), so a chroma block can be predicted from its
// co-located reconstructed luma block with a linear model
//   chroma_pred = a * luma_recon + b
// instead of from its own chroma neighbours. The (a, b) are derived
// from the neighbouring reconstructed luma/chroma samples (above row
// + left column), so nothing per block is signalled -- only the mode
// itself (PredMode::CCLM). Both encoder and decoder derive identical
// (a, b) from identical reconstructed neighbours.
//
// The fit is integer fixed-point (Q8 slope and offset) so encoder and
// decoder agree bit-for-bit on every platform. The slope is clamped
// to [-2, 2]; when the neighbour luma variance is ~0 (flat neighbours)
// the slope is 0 and the model reduces to the neighbour chroma mean.
//
// YUV444: the co-located luma block is 8x8, used directly.
// YUV420: the co-located luma is 16x16; it is downsampled to 8x8 by
// averaging each 2x2, and the neighbour luma samples are downsampled
// the same way (the above chroma row corresponds to luma rows
// 2*by0-2..2*by0-1 averaged; the left chroma column to luma cols
// 2*bx0-2..2*bx0-1 averaged).
#pragma once

#include <cstdint>

#include "common.h"

namespace wk {

// Fixed-point (Q8) linear model: chroma = (a * luma + b) >> 8.
struct CclmParams {
    int32_t a = 0;  // slope in Q8, clamped to [-512, 512] (i.e. [-2, 2])
    int32_t b = 0;  // offset in Q8 (pixel value * 256)
};

// Derives (a, b) from neighbour pairs by integer least squares.
// Up to 16 pairs: the 8 above samples and the 8 left samples
// (plus the corner, which is shared). A null pointer means that side
// has no neighbour; at least one side must be present. When the
// neighbour luma variance is ~0 the slope is 0 and b is the neighbour
// chroma mean (in Q8).
CclmParams derive_cclm_params(const int16_t* luma_above,
                              const int16_t* chroma_above,
                              const int16_t* luma_left,
                              const int16_t* chroma_left,
                              int16_t above_left_luma,
                              int16_t above_left_chroma,
                              bool has_above, bool has_left);

// Applies the model to a co-located reconstructed luma 8x8 block:
// pred[i] = clamp((a * luma_recon[i] + b) >> 8, 0, max_val).
void predict_cclm(const int16_t* luma_recon, const CclmParams& params,
                  int16_t* pred, int16_t max_val);

// Downsamples a 16x16 luma block (row-major, stride 16) to 8x8 by
// averaging each 2x2. Used for the YUV420 co-located luma.
void downsample_luma_16x16_to_8x8(const int16_t* luma_16, int16_t* luma_8);

// Computes the CCLM prediction for one chroma block from the tile's
// reconstructed planes. This is the single entry point the encoder
// and decoder use, so both derive identical (a, b) and predictions.
//   luma_plane: the tile's reconstructed luma (tw x th).
//   chroma_plane: the tile's reconstructed chroma (chroma_tw x chroma_th).
//   bx, by: the chroma block position within the tile.
//   subsampling: YUV444 (co-located luma is 8x8) or YUV420 (16x16,
//     downsampled to 8x8; neighbour luma downsampled the same way).
// The neighbours are the already-reconstructed above row and left
// column of this chroma block (and the corner). For a block in the
// first row/column the missing side is simply not used; for the
// top-left corner block (no neighbours at all) the model is flat
// mid-grey.
void predict_chroma_block_cclm(const int16_t* luma_plane, uint32_t tw,
                               uint32_t th, const int16_t* chroma_plane,
                               uint32_t chroma_tw, uint32_t chroma_th,
                               uint32_t bx, uint32_t by,
                               ChromaSubsampling subsampling, int16_t* pred,
                               int16_t max_val);

}  // namespace wk
