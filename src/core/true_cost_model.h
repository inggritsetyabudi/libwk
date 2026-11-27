// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "coeff_span_stream.h"
#include "common.h"
#include "dct.h"
#include "predict.h"
#include <array>
#include <cstdint>
#include <vector>

namespace wk {

// #rdo: Shannon true-cost model for RDO, built from one tile's pass-1
// blocks — the same model the #8 experiment measured the 2.09% ceiling
// with (variant A: per-zigzag-position magnitude tables, split signs,
// no significance maps). Prices mode + span + coefficients; unseen
// symbols cost -log2(0.5/N), as in the experiment.
struct TrueCostModel {
    static constexpr int kNumModes = 15;
    static constexpr int kMaxSpan = 65;

    float mode_cost[kNumModes]{};
    float span_cost[kMaxSpan]{};
    // mag_cost[pos][|v|]; beyond the vector, unseen_mag_cost[pos] applies.
    std::array<std::vector<float>, 64> mag_cost{};
    std::array<float, 64> unseen_mag_cost{};

    // Build from one tile's blocks; modes/spans parallel to blocks.
    // For chroma, pass the JOINT spans (max of Cb/Cr) and joint modes,
    // as the experiment did (both planes coded up to the joint span).
    // shrink_beta (iterasi 1, #rdo): per-position magnitude tables are
    // blended with the per-band pooled prior (bands {0},{1..3},{4..7},
    // {8..15},{16..31},{32..63}, as in experiment variant B):
    // p'(v) = (1-beta)*p_emp(v|pos) + beta*p_band(v). 0 = v1 behaviour
    // (peaked per-tile tables); >0 bounds the penalty of symbols that
    // are atypical at one position but common in the band, which is what
    // flattened smooth gradients one level in v1. Mode/span tables are
    // shrunk separately by mode_span_beta toward the uniform prior
    // (sweep result: magnitude shrinkage alone fixed WCG but left the
    // ramp and edge scenes bit-identical, so the binding penalty there
    // is in mode/span, not magnitudes). dc_beta shrinks the DC
    // magnitude (position 0, whose band is just itself) toward a flat
    // prior over the observed DC range: on a smooth tile every DC value
    // is rare (1-3 of ~1000 blocks), so an unseen DC costs ~11 bits vs
    // ~8-10 for a seen one, and that 1-3 bit gap can tip a marginal
    // block the wrong way; the flat prior compresses DC costs to
    // ~log2(range).
    void build(const std::vector<PredMode>& modes,
               const std::vector<uint8_t>& spans,
               const std::vector<DctBlockI16>& blocks,
               float shrink_beta = 0.0f, float mode_span_beta = 0.0f,
               float dc_beta = 0.0f);

    // Coefficient-stream bits of one quantized block coded up to `span`
    // (magnitude symbol per active position + 1 sign bit per nonzero).
    [[nodiscard]] float coeff_bits(const DctBlockI16& q, uint8_t span) const;
    // Full block bits: mode + span (from the block itself) + coefficients.
    [[nodiscard]] float block_bits(const DctBlockI16& q, PredMode mode) const;
};

}  // namespace wk
