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

#include <cstdint>
#include <optional>
#include <span>

#include "common.h"
#include "rans.h"

namespace wk {

// Parametric M3 coefficient-table entry (#m3): a fitted model of the
// magnitude distribution (zero + free m=1 frequency + geometric tail),
// serialized as 8 bytes in the coefficient-table bank (tag 7):
//   tag u8 (= 7), z u16 LE, p1 u16 LE, r u8, max_mag u16 LE.
// z_q / 4096 is the fraction of zero symbols, p1_q / 4096 the fraction
// of magnitude-1 symbols, r_q / 255 the geometric tail ratio, and
// max_mag the largest observed magnitude. The decoder rebuilds the
// exact same table from these quantized parameters, so the encoder and
// the decoder always agree on the table.
struct ParametricM3 {
    uint16_t z_q = 0;
    uint16_t p1_q = 0;
    uint8_t r_q = 0;
    uint16_t max_mag = 0;

    bool operator==(const ParametricM3&) const = default;
};

// Fit M3 to symbol counts. `limit` is the magnitude limit
// (coeff_symbol_offset(bit_depth)); `split` selects split
// magnitude/sign coding (symbols are magnitudes) versus legacy signed
// coding (symbols are value + limit, magnitudes are |value|).
// Returns nullopt when the counts are empty.
[[nodiscard]] std::optional<ParametricM3> fit_parametric_m3(
    std::span<const uint32_t> symbol_counts, int limit, bool split);

// Rebuild the table the decoder derives from the parameters. Pure and
// deterministic: the same parameters always produce the same table.
// Magnitudes up to max_mag are floored to frequency >= 1 so every
// observed symbol stays covered; the floor stops once the table holds
// TABLE_SIZE nonzero symbols, which keeps build_from_counts safe.
[[nodiscard]] RansTable<RANS_PRECISION_BITS> reconstruct_parametric_m3(const ParametricM3& params,
                                                       int limit, bool split);

// Write one parametric entry: tag 7 followed by the 7 parameter bytes.
void write_parametric_m3_entry(ByteWriter& writer, const ParametricM3& params);

// Read the 7 parameter bytes after a tag-7 byte. Rejects out-of-range
// parameters (z_q / p1_q > 4096, z_q + p1_q > 4096, max_mag > limit)
// and truncated input.
[[nodiscard]] Result<ParametricM3> read_parametric_m3_params(ByteReader& reader,
                                                            int limit,
                                                            std::string_view label);

}
