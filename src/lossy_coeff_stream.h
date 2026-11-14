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
#pragma once

#include "common.h"
#include "dct.h"

namespace wk {

// Coefficient symbol range is bit-depth aware. A quantized DCT
// coefficient scales with the pixel range (the DC of a constant block is
// 8x the residual), so the stream must be able to carry larger magnitudes
// at higher bit depths: 8-bit keeps the historical +-1024 range, 10-bit
// uses +-4096, 12-bit uses +-16384. The 8-bit range is unchanged, so
// 8-bit files are byte-identical to before. The shift is clamped so an
// unexpected bit depth can never produce a huge or negative range.
[[nodiscard]] inline int coeff_symbol_offset(uint8_t bit_depth) {
    const int shift = std::clamp(static_cast<int>(bit_depth) - 8, 0, 4);
    return 1024 << shift;
}

// The coefficient stream's symbol range is bit-depth aware (see
// coeff_symbol_offset above): a quantized coefficient outside that range
// reaches the decoder clamped, while the encoder's in-loop reconstruction
// would use the unclamped value -- so prediction for every following
// block would drift from what the decoder computes. Clamp here, once,
// so the stored coefficients, the reconstruction, and the stream all
// agree on the same values.
inline void clamp_quantized_to_stream_range(DctBlockI16& quantized, uint8_t bit_depth) {
    const int16_t limit = static_cast<int16_t>(coeff_symbol_offset(bit_depth));
    for (int i = 0; i < 64; ++i) {
        quantized[i] = std::clamp<int16_t>(quantized[i], static_cast<int16_t>(-limit), limit);
    }
}

struct LossyCoeffStreamConfig {
    bool use_plane_max_coeff_span = false;
    bool adaptive_coefficient_tables = true;
    bool split_magnitude_signs = false;
    bool use_table_bank = false;
    bool use_table_cluster_selection = false;
    bool elide_single_symbol_streams = false;
    bool use_significance_maps = false;
    bool use_adaptive_sign_streams = false;
    // Bit depth of the image being coded (8, 10 or 12). Drives the
    // coefficient symbol range via coeff_symbol_offset().
    uint8_t bit_depth = 8;
};

struct DecodedLossyChromaPayload {
    std::vector<DctBlockI16> cb_blocks;
    std::vector<DctBlockI16> cr_blocks;
};

// Byte breakdown of one lossy coefficient payload, for the Phase 1
// syntax-efficiency work: it tells which part of the payload dominates
// (the rANS frequency tables, the rANS-coded coefficient data, or the
// small flag streams), so an optimization targets the biggest piece.
struct LossyCoeffPayloadStats {
    size_t flag_bytes = 0;   // significance / sign mode flags
    size_t table_bytes = 0;  // rANS frequency tables (or table bank)
    size_t stream_bytes = 0; // rANS-coded coefficient data
    size_t total_bytes = 0;  // whole payload (== flag + table + stream)
};

[[nodiscard]] Result<std::vector<uint8_t>> encode_lossy_plane_payload(
    std::span<const DctBlockI16> blocks,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config,
    LossyCoeffPayloadStats* stats = nullptr);

[[nodiscard]] Result<std::vector<uint8_t>> encode_lossy_chroma_payload(
    std::span<const DctBlockI16> cb_blocks,
    std::span<const DctBlockI16> cr_blocks,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config,
    LossyCoeffPayloadStats* stats = nullptr);

[[nodiscard]] Result<std::vector<DctBlockI16>> decode_lossy_plane_payload(
    ByteReader& reader,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config);

[[nodiscard]] Result<DecodedLossyChromaPayload> decode_lossy_chroma_payload(
    ByteReader& reader,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config);

}
