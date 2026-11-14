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
#include "predict.h"

namespace wk {

// Layout V8 (Phase 1): like V7, but the per-block prediction modes and
// coefficient spans are rANS-coded instead of packed. Modes were 4 bits
// each (packed) and spans ~1 byte each; both distributions are skewed
// (a few modes and small spans dominate), so entropy coding them saves
// 14-24% of a tile's bytes. The rANS frequency table is stored with the
// same compact encoding as coefficient tables, so the overhead is small.
// The layout tag is defined in mode_stream.h (kLossyTileLayoutTagV8).

// Number of symbols in each rANS stream.
constexpr int kRansModeSymbolCount = static_cast<int>(PredMode::NUM_MODES);
constexpr int kRansSpanSymbolCount = 256;

// rANS-encode a mode stream: frequency table + rANS-coded symbols.
// The table uses the coefficient-table encoding (dense/sparse/delta),
// so sparse distributions stay compact.
[[nodiscard]] Result<void> write_rans_prediction_modes(ByteWriter& writer,
                                                      std::span<const PredMode> modes);
[[nodiscard]] Result<std::vector<PredMode>> read_rans_prediction_modes(ByteReader& reader,
                                                                      size_t expected_count,
                                                                      std::string_view label);

// rANS-encode a coefficient-span stream (one u8 span per block).
[[nodiscard]] Result<void> write_rans_coefficient_spans(ByteWriter& writer,
                                                        std::span<const uint8_t> spans);
[[nodiscard]] Result<std::vector<uint8_t>> read_rans_coefficient_spans(ByteReader& reader,
                                                                      size_t expected_count,
                                                                      std::string_view label);

}
