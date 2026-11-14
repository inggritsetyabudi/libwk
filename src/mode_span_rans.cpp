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

#include "mode_span_rans.h"

#include "coeff_table_stream.h"
#include "rans.h"

namespace wk {

namespace {

// Build a rANS table from symbol counts. Counts are the raw occurrence
// counts; build_from_counts normalizes them to sum to TABLE_SIZE.
[[nodiscard]] LossyCoeffTable build_table(const std::vector<uint32_t>& counts, int num_symbols) {
    LossyCoeffTable table;
    table.build_from_counts(counts.data(), num_symbols);
    return table;
}

// rANS-encode symbols (in reverse, as the encoder requires) and write
// the table + size-prefixed stream. Shared by modes and spans.
[[nodiscard]] Result<void> write_rans_stream(ByteWriter& writer,
                                             const std::vector<uint32_t>& counts,
                                             int num_symbols,
                                             const std::vector<int>& symbols,
                                             std::string_view label) {
    if (symbols.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     std::string(label) + " rANS stream is empty"});
    }
    LossyCoeffTable table = build_table(counts, num_symbols);
    auto table_result = write_coefficient_table(writer, table, num_symbols, nullptr);
    if (!table_result) {
        return std::unexpected(table_result.error());
    }
    RansEncoder<RANS_PRECISION_BITS> encoder;
    encoder.init();
    for (size_t i = symbols.size(); i > 0; --i) {
        encoder.encode(table, symbols[i - 1]);
    }
    auto bytes = encoder.finish();
    writer.write_u32(static_cast<uint32_t>(bytes.size()));
    writer.write_bytes(bytes);
    return {};
}

// Read a rANS table + size-prefixed stream and decode `count` symbols.
[[nodiscard]] Result<std::vector<int>> read_rans_stream(ByteReader& reader, int num_symbols,
                                                        size_t count, std::string_view label) {
    auto table = read_coefficient_table(reader, num_symbols, label, nullptr);
    if (!table) {
        return std::unexpected(table.error());
    }
    auto size = reader.read_u32();
    if (!size) {
        return std::unexpected(size.error());
    }
    auto encoded = reader.read_bytes(*size);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    RansDecoder<RANS_PRECISION_BITS> decoder;
    decoder.init(encoded->data(), encoded->size());
    if (!decoder.ok()) {
        return std::unexpected(Error{ErrorCode::RansError,
                                     std::string(label) + " invalid rANS stream header"});
    }
    std::vector<int> symbols;
    symbols.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const int symbol = decoder.decode(*table);
        if (!decoder.ok()) {
            return std::unexpected(Error{ErrorCode::RansError,
                                         std::string(label) + " corrupt rANS stream"});
        }
        symbols.push_back(symbol);
    }
    return symbols;
}

}  // namespace

Result<void> write_rans_prediction_modes(ByteWriter& writer, std::span<const PredMode> modes) {
    if (modes.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "mode stream is empty"});
    }
    std::vector<uint32_t> counts(static_cast<size_t>(kRansModeSymbolCount), 0);
    std::vector<int> symbols;
    symbols.reserve(modes.size());
    for (const auto mode : modes) {
        const int v = static_cast<int>(mode);
        if (v < 0 || v >= kRansModeSymbolCount) {
            return std::unexpected(Error{ErrorCode::InvalidParameter, "prediction mode out of range"});
        }
        counts[static_cast<size_t>(v)]++;
        symbols.push_back(v);
    }
    return write_rans_stream(writer, counts, kRansModeSymbolCount, symbols, "mode");
}

Result<std::vector<PredMode>> read_rans_prediction_modes(ByteReader& reader, size_t expected_count,
                                                         std::string_view label) {
    auto symbols = read_rans_stream(reader, kRansModeSymbolCount, expected_count, label);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    std::vector<PredMode> modes;
    modes.reserve(symbols->size());
    for (const int v : *symbols) {
        if (v < 0 || v >= kRansModeSymbolCount) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         std::string(label) + " mode symbol out of range"});
        }
        modes.push_back(static_cast<PredMode>(v));
    }
    return modes;
}

Result<void> write_rans_coefficient_spans(ByteWriter& writer, std::span<const uint8_t> spans) {
    if (spans.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "span stream is empty"});
    }
    std::vector<uint32_t> counts(static_cast<size_t>(kRansSpanSymbolCount), 0);
    std::vector<int> symbols;
    symbols.reserve(spans.size());
    for (const uint8_t span : spans) {
        counts[span]++;
        symbols.push_back(static_cast<int>(span));
    }
    return write_rans_stream(writer, counts, kRansSpanSymbolCount, symbols, "span");
}

Result<std::vector<uint8_t>> read_rans_coefficient_spans(ByteReader& reader, size_t expected_count,
                                                         std::string_view label) {
    auto symbols = read_rans_stream(reader, kRansSpanSymbolCount, expected_count, label);
    if (!symbols) {
        return std::unexpected(symbols.error());
    }
    std::vector<uint8_t> spans;
    spans.reserve(symbols->size());
    for (const int v : *symbols) {
        if (v < 0 || v >= kRansSpanSymbolCount) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         std::string(label) + " span symbol out of range"});
        }
        spans.push_back(static_cast<uint8_t>(v));
    }
    return spans;
}

}
