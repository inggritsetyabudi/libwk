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
#include "lossy_coeff_stream.h"

#include "coeff_presence_stream.h"
#include "coeff_table_bank_stream.h"
#include "coeff_sign_stream.h"
#include "coeff_table_stream.h"
#include "rans.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace wk {

namespace {

constexpr size_t kTableClusterCandidateLimit = 8;
constexpr size_t kTableClusterMaxPasses = 2;

struct SymbolCoding {
    int offset = 1024;
    int num_symbols = 2049;
    bool split_magnitude_signs = false;

    [[nodiscard]] int encode(int value) const {
        if (split_magnitude_signs) {
            return std::clamp(std::abs(value), 0, offset);
        }
        return std::clamp(value + offset, 0, num_symbols - 1);
    }

    [[nodiscard]] int16_t decode(int symbol) const {
        if (split_magnitude_signs) {
            return static_cast<int16_t>(symbol);
        }
        return static_cast<int16_t>(symbol - offset);
    }
};

struct EncodedCoeffContextStream {
    bool use_significance_map = false;
    std::vector<uint8_t> presence_bytes;
    std::vector<uint8_t> rans_bytes;
    std::vector<uint8_t> sign_bits;
    uint8_t sign_mode = kCoefficientSignModeRawPacked;
    size_t active_blocks = 0;
    size_t nonzero_count = 0;
};

struct CoeffContextAnalysis {
    std::vector<uint8_t> presence;
    size_t active_blocks = 0;
    size_t nonzero_count = 0;
};

[[nodiscard]] Result<void> write_lossy_coeff_table(ByteWriter& writer,
                                                   const LossyCoeffTable& table,
                                                   int num_symbols,
                                                   const LossyCoeffStreamConfig& config,
                                                   const LossyCoeffTable* previous_table = nullptr);
[[nodiscard]] Result<void> write_coeff_context_stream(ByteWriter& writer,
                                                      const EncodedCoeffContextStream& stream,
                                                      const LossyCoeffTable& table,
                                                      const SymbolCoding& coding,
                                                      const LossyCoeffStreamConfig& config);

[[nodiscard]] SymbolCoding make_symbol_coding(const LossyCoeffStreamConfig& config) {
    SymbolCoding coding;
    coding.split_magnitude_signs = config.split_magnitude_signs;
    coding.offset = coeff_symbol_offset(config.bit_depth);
    coding.num_symbols = config.split_magnitude_signs ? coding.offset + 1
                                                       : 2 * coding.offset + 1;
    return coding;
}

[[nodiscard]] int coefficient_limit(uint8_t max_coeff_span, const LossyCoeffStreamConfig& config) {
    return config.use_plane_max_coeff_span ? max_coeff_span : 64;
}

[[nodiscard]] size_t count_active_blocks(std::span<const uint8_t> spans, int coeff_index) {
    size_t active = 0;
    for (uint8_t span_value : spans) {
        active += span_value > coeff_index ? 1u : 0u;
    }
    return active;
}

[[nodiscard]] CoeffContextAnalysis analyze_coeff_context(std::span<const DctBlockI16> blocks,
                                                         std::span<const uint8_t> spans,
                                                         int coeff_index,
                                                         bool keep_presence) {
    CoeffContextAnalysis analysis;
    if (keep_presence) {
        analysis.presence.reserve(blocks.size());
    }

    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        if (spans[block_index] <= coeff_index) {
            continue;
        }
        const bool nonzero = blocks[block_index][coeff_index] != 0;
        if (keep_presence) {
            analysis.presence.push_back(static_cast<uint8_t>(nonzero));
        }
        ++analysis.active_blocks;
        analysis.nonzero_count += nonzero ? 1u : 0u;
    }

    return analysis;
}

[[nodiscard]] Result<void> write_significance_mode_flags(ByteWriter& writer,
                                                         std::span<const uint8_t> flags) {
    auto packed = pack_coefficient_presence(flags);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    writer.write_bytes(*packed);
    return {};
}

[[nodiscard]] Result<std::vector<uint8_t>> read_significance_mode_flags(ByteReader& reader,
                                                                        size_t count) {
    const size_t packed_size = packed_coefficient_presence_bytes(count);
    auto packed = reader.read_bytes(packed_size);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    return unpack_coefficient_presence(*packed, count, "lossy significance mode");
}

[[nodiscard]] Result<void> write_sign_mode_flags(ByteWriter& writer,
                                                 std::span<const uint8_t> flags) {
    auto packed = pack_coefficient_sign_modes(flags);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    writer.write_bytes(*packed);
    return {};
}

[[nodiscard]] Result<std::vector<uint8_t>> read_sign_mode_flags(ByteReader& reader, size_t count) {
    const size_t packed_size = packed_coefficient_sign_mode_bytes(count);
    auto packed = reader.read_bytes(packed_size);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    return unpack_coefficient_sign_modes(*packed, count, "lossy coefficient sign mode");
}

[[nodiscard]] uint8_t select_sign_mode(std::span<const uint8_t> signs,
                                       bool use_adaptive_sign_streams) {
    if (!use_adaptive_sign_streams || signs.empty()) {
        return kCoefficientSignModeRawPacked;
    }
    if (std::all_of(signs.begin(), signs.end(),
                    [](uint8_t sign) { return sign == 0; })) {
        return kCoefficientSignModeAllPositive;
    }
    if (std::all_of(signs.begin(), signs.end(),
                    [](uint8_t sign) { return sign == 1; })) {
        return kCoefficientSignModeAllNegative;
    }
    return kCoefficientSignModeRawPacked;
}

[[nodiscard]] Result<size_t> serialized_coeff_context_stream_size(
    const EncodedCoeffContextStream& stream,
    const LossyCoeffTable& table,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config) {
    ByteWriter writer;
    auto written = write_coeff_context_stream(writer, stream, table, coding, config);
    if (!written) {
        return std::unexpected(written.error());
    }
    return writer.size();
}

[[nodiscard]] Result<size_t> serialized_lossy_coeff_table_size(const LossyCoeffTable& table,
                                                               int num_symbols,
                                                               const LossyCoeffStreamConfig& config,
                                                               const LossyCoeffTable* previous_table) {
    ByteWriter writer;
    auto written = write_lossy_coeff_table(writer, table, num_symbols, config, previous_table);
    if (!written) {
        return std::unexpected(written.error());
    }
    return writer.size();
}

[[nodiscard]] LossyCoeffTable make_single_symbol_table(const SymbolCoding& coding) {
    std::vector<uint32_t> counts(static_cast<size_t>(coding.num_symbols), 0);
    counts[static_cast<size_t>(coding.split_magnitude_signs ? 0 : coding.offset)] = 1;
    LossyCoeffTable table;
    table.build_from_counts(counts.data(), coding.num_symbols);
    return table;
}

[[nodiscard]] std::optional<int> single_symbol_from_table(const LossyCoeffTable& table,
                                                          int num_symbols) {
    int symbol_index = -1;
    for (int symbol = 0; symbol < num_symbols; ++symbol) {
        const uint16_t freq = table.symbol(symbol).freq;
        if (freq == 0) {
            continue;
        }
        if (freq != LossyCoeffTable::TABLE_SIZE || symbol_index >= 0) {
            return std::nullopt;
        }
        symbol_index = symbol;
    }
    if (symbol_index < 0) {
        return std::nullopt;
    }
    return symbol_index;
}

[[nodiscard]] Result<void> write_legacy_coeff_table(ByteWriter& writer,
                                                    const LossyCoeffTable& table,
                                                    int num_symbols) {
    int first_nonzero = -1;
    int last_nonzero = -1;
    for (int symbol = 0; symbol < num_symbols; ++symbol) {
        if (table.symbol(symbol).freq == 0) {
            continue;
        }
        if (first_nonzero < 0) {
            first_nonzero = symbol;
        }
        last_nonzero = symbol;
    }

    if (first_nonzero < 0) {
        return std::unexpected(
            Error{ErrorCode::InvalidParameter, "coefficient table must contain at least one symbol"});
    }

    writer.write_u16(static_cast<uint16_t>(first_nonzero));
    writer.write_u16(static_cast<uint16_t>(last_nonzero));
    for (int symbol = first_nonzero; symbol <= last_nonzero; ++symbol) {
        writer.write_u16(table.symbol(symbol).freq);
    }
    return {};
}

[[nodiscard]] Result<void> write_lossy_coeff_table(ByteWriter& writer,
                                                   const LossyCoeffTable& table,
                                                   int num_symbols,
                                                   const LossyCoeffStreamConfig& config,
                                                   const LossyCoeffTable* previous_table) {
    if (config.adaptive_coefficient_tables) {
        return write_coefficient_table(writer, table, num_symbols, previous_table);
    }
    return write_legacy_coeff_table(writer, table, num_symbols);
}

[[nodiscard]] Result<EncodedCoeffContextStream> encode_coeff_context_stream(
    std::span<const DctBlockI16> blocks,
    std::span<const uint8_t> spans,
    int coeff_index,
    const CoeffContextAnalysis& analysis,
    const LossyCoeffTable& table,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config,
    bool use_significance_map) {
    EncodedCoeffContextStream stream;
    stream.use_significance_map = use_significance_map;
    stream.active_blocks = analysis.active_blocks;
    stream.nonzero_count = use_significance_map ? analysis.nonzero_count : analysis.active_blocks;

    if (use_significance_map && analysis.active_blocks > 0) {
        ByteWriter presence_writer;
        auto written = write_adaptive_coefficient_presence(presence_writer, analysis.presence);
        if (!written) {
            return std::unexpected(written.error());
        }
        stream.presence_bytes = presence_writer.finish();
    }

    if (stream.nonzero_count == 0) {
        return stream;
    }

    const bool elide_rans_stream = config.elide_single_symbol_streams &&
        single_symbol_from_table(table, coding.num_symbols).has_value();

    if (!elide_rans_stream) {
        RansEncoder<RANS_PRECISION_BITS> encoder;
        encoder.init();

        for (size_t reverse_index = blocks.size(); reverse_index > 0; --reverse_index) {
            const size_t block_index = reverse_index - 1;
            if (spans[block_index] <= coeff_index) {
                continue;
            }
            const int value = static_cast<int>(blocks[block_index][coeff_index]);
            if (use_significance_map && value == 0) {
                continue;
            }
            encoder.encode(table, coding.encode(value));
        }
        stream.rans_bytes = encoder.finish();
    }

    if (coding.split_magnitude_signs) {
        std::vector<uint8_t> signs;
        signs.reserve(blocks.size());
        for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
            if (spans[block_index] <= coeff_index) {
                continue;
            }
            const int value = static_cast<int>(blocks[block_index][coeff_index]);
            if (value != 0) {
                signs.push_back(static_cast<uint8_t>(value < 0));
            }
        }
        stream.sign_mode = select_sign_mode(signs, config.use_adaptive_sign_streams);
        if (stream.sign_mode == kCoefficientSignModeRawPacked) {
            auto packed_signs = pack_coefficient_signs(signs);
            if (!packed_signs) {
                return std::unexpected(packed_signs.error());
            }
            stream.sign_bits = std::move(*packed_signs);
        }
    }

    return stream;
}

[[nodiscard]] Result<void> write_coeff_context_stream(
    ByteWriter& writer,
    const EncodedCoeffContextStream& stream,
    const LossyCoeffTable& table,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config) {
    if (stream.use_significance_map && stream.active_blocks > 0) {
        writer.write_bytes(stream.presence_bytes);
    }
    if (stream.nonzero_count == 0) {
        if (!stream.use_significance_map) {
            writer.write_u32(0);
        }
        return {};
    }

    const bool elide_rans_stream = config.elide_single_symbol_streams &&
        stream.nonzero_count > 0 &&
        single_symbol_from_table(table, coding.num_symbols).has_value();
    if (!elide_rans_stream) {
        writer.write_u32(static_cast<uint32_t>(stream.rans_bytes.size()));
        writer.write_bytes(stream.rans_bytes);
    }
    writer.write_bytes(stream.sign_bits);
    return {};
}

[[nodiscard]] Result<size_t> decode_elided_single_symbol_stream(
    ByteReader& reader,
    std::span<const uint8_t> spans,
    int coeff_index,
    const SymbolCoding& coding,
    int symbol,
    std::span<const uint8_t> presence,
    uint8_t sign_mode,
    std::span<DctBlockI16> blocks,
    std::string_view label) {
    const int16_t value = coding.decode(symbol);
    size_t present_blocks = 0;
    size_t presence_index = 0;
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        if (spans[block_index] <= coeff_index) {
            continue;
        }
        const bool is_present = presence.empty() || presence[presence_index++] != 0;
        if (!is_present) {
            blocks[block_index][coeff_index] = 0;
            continue;
        }
        blocks[block_index][coeff_index] = value;
        ++present_blocks;
    }

    if (coding.split_magnitude_signs && value != 0) {
        if (sign_mode == kCoefficientSignModeAllPositive) {
            return present_blocks;
        }
        if (sign_mode == kCoefficientSignModeAllNegative) {
            presence_index = 0;
            for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
                if (spans[block_index] <= coeff_index) {
                    continue;
                }
                const bool is_present = presence.empty() || presence[presence_index++] != 0;
                if (!is_present) {
                    continue;
                }
                blocks[block_index][coeff_index] = static_cast<int16_t>(-value);
            }
            return present_blocks;
        }
        if (sign_mode != kCoefficientSignModeRawPacked) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "lossy coefficient sign mode is invalid"});
        }

        auto sign_bits = read_packed_coefficient_signs(reader, present_blocks, label);
        if (!sign_bits) {
            return std::unexpected(sign_bits.error());
        }

        size_t sign_index = 0;
        presence_index = 0;
        for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
            if (spans[block_index] <= coeff_index) {
                continue;
            }
            const bool is_present = presence.empty() || presence[presence_index++] != 0;
            if (!is_present) {
                continue;
            }
            if ((*sign_bits)[sign_index++] != 0) {
                blocks[block_index][coeff_index] = static_cast<int16_t>(-value);
            }
        }
    }

    return present_blocks;
}

[[nodiscard]] Result<EncodedCoeffContextStream> read_coeff_context_stream(
    ByteReader& reader,
    std::span<const uint8_t> spans,
    int coeff_index,
    const LossyCoeffTable& table,
    const SymbolCoding& coding,
    std::span<DctBlockI16> blocks,
    std::string_view label,
    const LossyCoeffStreamConfig& config,
    bool use_significance_map,
    uint8_t sign_mode) {
    EncodedCoeffContextStream stream;
    const size_t active_blocks = count_active_blocks(spans, coeff_index);
    stream.active_blocks = active_blocks;
    stream.use_significance_map = use_significance_map;

    if (active_blocks == 0) {
        if (!use_significance_map) {
            auto encoded_size = reader.read_u32();
            if (!encoded_size) {
                return std::unexpected(encoded_size.error());
            }
            if (*encoded_size != 0) {
                return std::unexpected(Error{ErrorCode::RansError,
                                             "unexpected data for empty coefficient stream"});
            }
        }
        return stream;
    }

    std::vector<uint8_t> presence;
    if (use_significance_map) {
        auto presence_read = read_adaptive_coefficient_presence(reader, active_blocks, label);
        if (!presence_read) {
            return std::unexpected(presence_read.error());
        }
        presence = std::move(*presence_read);
        stream.nonzero_count = std::count(presence.begin(), presence.end(), static_cast<uint8_t>(1));
        if (stream.nonzero_count == 0) {
            return stream;
        }
    } else {
        stream.nonzero_count = active_blocks;
    }

    if (config.elide_single_symbol_streams) {
        auto symbol = single_symbol_from_table(table, coding.num_symbols);
        if (symbol) {
            auto decoded = decode_elided_single_symbol_stream(reader, spans, coeff_index, coding,
                                                              *symbol, presence, sign_mode, blocks,
                                                              label);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            stream.nonzero_count = *decoded;
            return stream;
        }
    }

    auto encoded_size = reader.read_u32();
    if (!encoded_size) {
        return std::unexpected(encoded_size.error());
    }
    auto encoded = reader.read_bytes(*encoded_size);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }

    RansDecoder<RANS_PRECISION_BITS> decoder;
    decoder.init(encoded->data(), encoded->size());
    if (!decoder.ok()) {
        return std::unexpected(Error{ErrorCode::RansError, "invalid rANS stream header"});
    }

    size_t nonzero_count = 0;
    size_t presence_index = 0;
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        if (spans[block_index] <= coeff_index) {
            continue;
        }
        if (!presence.empty() && presence[presence_index++] == 0) {
            blocks[block_index][coeff_index] = 0;
            continue;
        }
        const int symbol = decoder.decode(table);
        if (!decoder.ok()) {
            return std::unexpected(Error{ErrorCode::RansError, "corrupt rANS stream"});
        }
        blocks[block_index][coeff_index] = coding.decode(symbol);
        nonzero_count += blocks[block_index][coeff_index] != 0 ? 1u : 0u;
    }

    if (coding.split_magnitude_signs) {
        if (sign_mode == kCoefficientSignModeRawPacked) {
            auto sign_bits = read_packed_coefficient_signs(reader, nonzero_count, label);
            if (!sign_bits) {
                return std::unexpected(sign_bits.error());
            }

            size_t sign_index = 0;
            presence_index = 0;
            for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
                if (spans[block_index] <= coeff_index) {
                    continue;
                }
                if (!presence.empty() && presence[presence_index++] == 0) {
                    continue;
                }
                auto& value = blocks[block_index][coeff_index];
                if (value == 0) {
                    continue;
                }
                if ((*sign_bits)[sign_index++] != 0) {
                    value = static_cast<int16_t>(-value);
                }
            }
        } else if (sign_mode == kCoefficientSignModeAllNegative) {
            for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
                auto& value = blocks[block_index][coeff_index];
                if (value > 0) {
                    value = static_cast<int16_t>(-value);
                }
            }
        } else if (sign_mode != kCoefficientSignModeAllPositive) {
            return std::unexpected(Error{ErrorCode::DecodeFailed,
                                         "lossy coefficient sign mode is invalid"});
        }
    }

    return stream;
}

[[nodiscard]] Result<LossyCoeffTable> read_lossy_coeff_table(
    ByteReader& reader,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config,
    const LossyCoeffTable* previous_table) {
    if (config.adaptive_coefficient_tables) {
        return read_coefficient_table(reader, coding.num_symbols, "lossy", previous_table);
    }

    auto first_read = reader.read_u16();
    auto last_read = reader.read_u16();
    if (!first_read || !last_read) {
        return std::unexpected(Error{ErrorCode::TruncatedInput, "missing rANS symbol range"});
    }

    const int first = *first_read;
    const int last = *last_read;
    if (first > last || first < 0 || last >= coding.num_symbols) {
        return std::unexpected(Error{ErrorCode::RansError, "invalid rANS symbol range"});
    }

    std::vector<uint32_t> counts(static_cast<size_t>(coding.num_symbols), 0);
    for (int symbol = first; symbol <= last; ++symbol) {
        auto freq = reader.read_u16();
        if (!freq) {
            return std::unexpected(freq.error());
        }
        counts[static_cast<size_t>(symbol)] = *freq;
    }

    LossyCoeffTable table;
    table.build_from_counts(counts.data(), coding.num_symbols);
    return table;
}

[[nodiscard]] std::vector<uint32_t> build_plane_counts(
    std::span<const DctBlockI16> blocks,
    std::span<const uint8_t> spans,
    int coeff_index,
    const SymbolCoding& coding,
    bool skip_zeros) {
    std::vector<uint32_t> counts(static_cast<size_t>(coding.num_symbols), 0);
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        if (spans[block_index] <= coeff_index) {
            continue;
        }
        const int value = static_cast<int>(blocks[block_index][coeff_index]);
        if (skip_zeros && value == 0) {
            continue;
        }
        counts[static_cast<size_t>(coding.encode(value))]++;
    }
    return counts;
}

[[nodiscard]] std::vector<uint32_t> build_chroma_counts(
    std::span<const DctBlockI16> cb_blocks,
    std::span<const DctBlockI16> cr_blocks,
    std::span<const uint8_t> spans,
    int coeff_index,
    const SymbolCoding& coding,
    bool skip_zeros) {
    std::vector<uint32_t> counts(static_cast<size_t>(coding.num_symbols), 0);
    for (size_t block_index = 0; block_index < cb_blocks.size(); ++block_index) {
        if (spans[block_index] <= coeff_index) {
            continue;
        }
        const int cb_value = static_cast<int>(cb_blocks[block_index][coeff_index]);
        const int cr_value = static_cast<int>(cr_blocks[block_index][coeff_index]);
        if (!skip_zeros || cb_value != 0) {
            counts[static_cast<size_t>(coding.encode(cb_value))]++;
        }
        if (!skip_zeros || cr_value != 0) {
            counts[static_cast<size_t>(coding.encode(cr_value))]++;
        }
    }
    return counts;
}

[[nodiscard]] bool same_coeff_table(const LossyCoeffTable& lhs,
                                    const LossyCoeffTable& rhs,
                                    int num_symbols) {
    for (int symbol = 0; symbol < num_symbols; ++symbol) {
        if (lhs.symbol(symbol).freq != rhs.symbol(symbol).freq) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] uint64_t coeff_table_distance(const LossyCoeffTable& lhs,
                                            const LossyCoeffTable& rhs,
                                            int num_symbols) {
    uint64_t distance = 0;
    for (int symbol = 0; symbol < num_symbols; ++symbol) {
        const int lhs_freq = lhs.symbol(symbol).freq;
        const int rhs_freq = rhs.symbol(symbol).freq;
        distance += static_cast<uint64_t>(std::abs(lhs_freq - rhs_freq));
    }
    return distance;
}

[[nodiscard]] std::vector<size_t> collect_table_cluster_candidates(
    std::span<const LossyCoeffTable> tables,
    size_t current_index,
    int num_symbols) {
    struct Candidate {
        size_t index = 0;
        uint64_t distance = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(tables.size());
    for (size_t candidate_index = 0; candidate_index < tables.size(); ++candidate_index) {
        if (candidate_index == current_index) {
            continue;
        }
        if (same_coeff_table(tables[current_index], tables[candidate_index], num_symbols)) {
            continue;
        }

        bool already_listed = false;
        for (const Candidate& candidate : candidates) {
            if (same_coeff_table(tables[candidate.index], tables[candidate_index], num_symbols)) {
                already_listed = true;
                break;
            }
        }
        if (already_listed) {
            continue;
        }

        candidates.push_back(Candidate{
            .index = candidate_index,
            .distance = coeff_table_distance(tables[current_index],
                                             tables[candidate_index],
                                             num_symbols),
        });
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (lhs.distance != rhs.distance) {
                      return lhs.distance < rhs.distance;
                  }
                  return lhs.index < rhs.index;
              });

    if (candidates.size() > kTableClusterCandidateLimit) {
        candidates.resize(kTableClusterCandidateLimit);
    }

    std::vector<size_t> indices;
    indices.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        indices.push_back(candidate.index);
    }
    return indices;
}

[[nodiscard]] bool table_supports_counts(const LossyCoeffTable& table,
                                         std::span<const uint32_t> counts) {
    for (size_t symbol = 0; symbol < counts.size(); ++symbol) {
        if (counts[symbol] == 0) {
            continue;
        }
        if (table.symbol(static_cast<int>(symbol)).freq == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<size_t> serialized_plane_bank_payload_size(
    std::span<const LossyCoeffTable> tables,
    std::span<const EncodedCoeffContextStream> streams,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config) {
    ByteWriter writer;
    auto bank_result = write_coefficient_table_bank(writer, tables, coding.num_symbols);
    if (!bank_result) {
        return std::unexpected(bank_result.error());
    }
    for (size_t context_index = 0; context_index < tables.size(); ++context_index) {
        auto stream_result = write_coeff_context_stream(writer, streams[context_index],
                                                        tables[context_index], coding, config);
        if (!stream_result) {
            return std::unexpected(stream_result.error());
        }
    }
    return writer.size();
}

[[nodiscard]] Result<size_t> serialized_chroma_bank_payload_size(
    std::span<const LossyCoeffTable> tables,
    std::span<const EncodedCoeffContextStream> cb_streams,
    std::span<const EncodedCoeffContextStream> cr_streams,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config) {
    ByteWriter writer;
    auto bank_result = write_coefficient_table_bank(writer, tables, coding.num_symbols);
    if (!bank_result) {
        return std::unexpected(bank_result.error());
    }
    for (size_t context_index = 0; context_index < tables.size(); ++context_index) {
        auto cb_result = write_coeff_context_stream(writer, cb_streams[context_index],
                                                    tables[context_index], coding, config);
        if (!cb_result) {
            return std::unexpected(cb_result.error());
        }
        auto cr_result = write_coeff_context_stream(writer, cr_streams[context_index],
                                                    tables[context_index], coding, config);
        if (!cr_result) {
            return std::unexpected(cr_result.error());
        }
    }
    return writer.size();
}

[[nodiscard]] Result<void> optimize_plane_table_clusters(
    std::span<const DctBlockI16> blocks,
    std::span<const uint8_t> spans,
    std::span<LossyCoeffTable> tables,
    std::span<EncodedCoeffContextStream> streams,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config) {
    if (!config.use_table_bank || !config.use_table_cluster_selection || tables.size() < 2) {
        return {};
    }

    auto best_payload_size = serialized_plane_bank_payload_size(tables, streams, coding, config);
    if (!best_payload_size) {
        return std::unexpected(best_payload_size.error());
    }

    for (size_t pass = 0; pass < kTableClusterMaxPasses; ++pass) {
        bool changed = false;
        for (size_t context_index = 0; context_index < tables.size(); ++context_index) {
            const std::vector<size_t> candidate_indices =
                collect_table_cluster_candidates(tables, context_index, coding.num_symbols);
            if (candidate_indices.empty()) {
                continue;
            }

            const bool use_significance_map = streams[context_index].use_significance_map;
            const CoeffContextAnalysis analysis =
                analyze_coeff_context(blocks, spans, static_cast<int>(context_index),
                                      use_significance_map);
            if (analysis.active_blocks == 0) {
                continue;
            }
            const std::vector<uint32_t> counts = build_plane_counts(
                blocks, spans, static_cast<int>(context_index), coding, use_significance_map);

            const LossyCoeffTable original_table = tables[context_index];
            const EncodedCoeffContextStream original_stream = streams[context_index];
            LossyCoeffTable best_table = original_table;
            EncodedCoeffContextStream best_stream = original_stream;

            for (size_t candidate_index : candidate_indices) {
                if (!table_supports_counts(tables[candidate_index], counts)) {
                    continue;
                }
                auto candidate_stream = encode_coeff_context_stream(
                    blocks, spans, static_cast<int>(context_index), analysis,
                    tables[candidate_index], coding, config, use_significance_map);
                if (!candidate_stream) {
                    return std::unexpected(candidate_stream.error());
                }

                tables[context_index] = tables[candidate_index];
                streams[context_index] = std::move(*candidate_stream);
                auto trial_payload_size =
                    serialized_plane_bank_payload_size(tables, streams, coding, config);
                if (!trial_payload_size) {
                    return std::unexpected(trial_payload_size.error());
                }
                if (*trial_payload_size < *best_payload_size) {
                    *best_payload_size = *trial_payload_size;
                    best_table = tables[context_index];
                    best_stream = streams[context_index];
                    changed = true;
                }
            }

            tables[context_index] = best_table;
            streams[context_index] = std::move(best_stream);
        }

        if (!changed) {
            break;
        }
    }

    return {};
}

[[nodiscard]] Result<void> optimize_chroma_table_clusters(
    std::span<const DctBlockI16> cb_blocks,
    std::span<const DctBlockI16> cr_blocks,
    std::span<const uint8_t> spans,
    std::span<LossyCoeffTable> tables,
    std::span<EncodedCoeffContextStream> cb_streams,
    std::span<EncodedCoeffContextStream> cr_streams,
    const SymbolCoding& coding,
    const LossyCoeffStreamConfig& config) {
    if (!config.use_table_bank || !config.use_table_cluster_selection || tables.size() < 2) {
        return {};
    }

    auto best_payload_size =
        serialized_chroma_bank_payload_size(tables, cb_streams, cr_streams, coding, config);
    if (!best_payload_size) {
        return std::unexpected(best_payload_size.error());
    }

    for (size_t pass = 0; pass < kTableClusterMaxPasses; ++pass) {
        bool changed = false;
        for (size_t context_index = 0; context_index < tables.size(); ++context_index) {
            const std::vector<size_t> candidate_indices =
                collect_table_cluster_candidates(tables, context_index, coding.num_symbols);
            if (candidate_indices.empty()) {
                continue;
            }

            const bool use_significance_map = cb_streams[context_index].use_significance_map;
            const CoeffContextAnalysis cb_analysis =
                analyze_coeff_context(cb_blocks, spans, static_cast<int>(context_index),
                                      use_significance_map);
            const CoeffContextAnalysis cr_analysis =
                analyze_coeff_context(cr_blocks, spans, static_cast<int>(context_index),
                                      use_significance_map);
            if (cb_analysis.active_blocks == 0 && cr_analysis.active_blocks == 0) {
                continue;
            }
            const std::vector<uint32_t> counts = build_chroma_counts(
                cb_blocks, cr_blocks, spans, static_cast<int>(context_index), coding,
                use_significance_map);

            const LossyCoeffTable original_table = tables[context_index];
            const EncodedCoeffContextStream original_cb_stream = cb_streams[context_index];
            const EncodedCoeffContextStream original_cr_stream = cr_streams[context_index];
            LossyCoeffTable best_table = original_table;
            EncodedCoeffContextStream best_cb_stream = original_cb_stream;
            EncodedCoeffContextStream best_cr_stream = original_cr_stream;

            for (size_t candidate_index : candidate_indices) {
                if (!table_supports_counts(tables[candidate_index], counts)) {
                    continue;
                }
                auto candidate_cb_stream = encode_coeff_context_stream(
                    cb_blocks, spans, static_cast<int>(context_index), cb_analysis,
                    tables[candidate_index], coding, config, use_significance_map);
                if (!candidate_cb_stream) {
                    return std::unexpected(candidate_cb_stream.error());
                }

                auto candidate_cr_stream = encode_coeff_context_stream(
                    cr_blocks, spans, static_cast<int>(context_index), cr_analysis,
                    tables[candidate_index], coding, config, use_significance_map);
                if (!candidate_cr_stream) {
                    return std::unexpected(candidate_cr_stream.error());
                }

                tables[context_index] = tables[candidate_index];
                cb_streams[context_index] = std::move(*candidate_cb_stream);
                cr_streams[context_index] = std::move(*candidate_cr_stream);
                auto trial_payload_size = serialized_chroma_bank_payload_size(
                    tables, cb_streams, cr_streams, coding, config);
                if (!trial_payload_size) {
                    return std::unexpected(trial_payload_size.error());
                }
                if (*trial_payload_size < *best_payload_size) {
                    *best_payload_size = *trial_payload_size;
                    best_table = tables[context_index];
                    best_cb_stream = cb_streams[context_index];
                    best_cr_stream = cr_streams[context_index];
                    changed = true;
                }
            }

            tables[context_index] = best_table;
            cb_streams[context_index] = std::move(best_cb_stream);
            cr_streams[context_index] = std::move(best_cr_stream);
        }

        if (!changed) {
            break;
        }
    }

    return {};
}

}

Result<std::vector<uint8_t>> encode_lossy_plane_payload(
    std::span<const DctBlockI16> blocks,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config,
    LossyCoeffPayloadStats* stats) {
    if (config.use_table_bank && !config.adaptive_coefficient_tables) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     "coefficient table bank requires adaptive coefficient tables"});
    }
    if (blocks.size() != spans.size()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     "lossy plane coefficient blocks and spans must match"});
    }

    const SymbolCoding coding = make_symbol_coding(config);
    const int coeff_limit = coefficient_limit(max_coeff_span, config);
    ByteWriter writer;
    std::vector<LossyCoeffTable> tables;
    std::vector<EncodedCoeffContextStream> streams;
    std::vector<uint8_t> significance_flags;
    std::vector<uint8_t> sign_modes;
    tables.reserve(static_cast<size_t>(coeff_limit));
    streams.reserve(static_cast<size_t>(coeff_limit));
    if (config.use_significance_maps) {
        significance_flags.reserve(static_cast<size_t>(coeff_limit));
    }
    if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
        sign_modes.reserve(static_cast<size_t>(coeff_limit));
    }

    LossyCoeffStreamConfig raw_config = config;
    raw_config.use_significance_maps = false;
    LossyCoeffStreamConfig sparse_config = config;
    sparse_config.use_significance_maps = true;

    for (int coeff_index = 0; coeff_index < coeff_limit; ++coeff_index) {
        const CoeffContextAnalysis analysis =
            analyze_coeff_context(blocks, spans, coeff_index, config.use_significance_maps);
        if (analysis.active_blocks == 0) {
            tables.push_back(make_single_symbol_table(coding));
            EncodedCoeffContextStream empty_stream;
            streams.push_back(std::move(empty_stream));
            if (config.use_significance_maps) {
                significance_flags.push_back(0);
            }
            if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
                sign_modes.push_back(kCoefficientSignModeRawPacked);
            }
            continue;
        }

        auto raw_counts = build_plane_counts(blocks, spans, coeff_index, coding, false);
        LossyCoeffTable raw_table;
        raw_table.build_from_counts(raw_counts.data(), coding.num_symbols);
        auto raw_stream = encode_coeff_context_stream(blocks, spans, coeff_index, analysis,
                                                      raw_table, coding, raw_config, false);
        if (!raw_stream) {
            return std::unexpected(raw_stream.error());
        }

        bool use_sparse_mode = false;
        LossyCoeffTable selected_table = raw_table;
        EncodedCoeffContextStream selected_stream = std::move(*raw_stream);
        const LossyCoeffTable* previous_table =
            (!config.use_table_bank && !tables.empty()) ? &tables.back() : nullptr;

        if (config.use_significance_maps) {
            auto sparse_counts = build_plane_counts(blocks, spans, coeff_index, coding, true);
            LossyCoeffTable sparse_table;
            if (analysis.nonzero_count == 0) {
                sparse_table = make_single_symbol_table(coding);
            } else {
                sparse_table.build_from_counts(sparse_counts.data(), coding.num_symbols);
            }
            auto sparse_stream = encode_coeff_context_stream(blocks, spans, coeff_index, analysis,
                                                             sparse_table, coding, sparse_config, true);
            if (!sparse_stream) {
                return std::unexpected(sparse_stream.error());
            }

            auto raw_table_size = serialized_lossy_coeff_table_size(raw_table, coding.num_symbols, raw_config,
                                                                    previous_table);
            if (!raw_table_size) {
                return std::unexpected(raw_table_size.error());
            }
            auto raw_stream_size = serialized_coeff_context_stream_size(*raw_stream, raw_table, coding, raw_config);
            if (!raw_stream_size) {
                return std::unexpected(raw_stream_size.error());
            }
            auto sparse_table_size = serialized_lossy_coeff_table_size(sparse_table, coding.num_symbols, sparse_config,
                                                                       previous_table);
            if (!sparse_table_size) {
                return std::unexpected(sparse_table_size.error());
            }
            auto sparse_stream_size = serialized_coeff_context_stream_size(*sparse_stream, sparse_table, coding, sparse_config);
            if (!sparse_stream_size) {
                return std::unexpected(sparse_stream_size.error());
            }

            use_sparse_mode = *sparse_table_size + *sparse_stream_size < *raw_table_size + *raw_stream_size;
            if (use_sparse_mode) {
                selected_table = std::move(sparse_table);
                selected_stream = std::move(*sparse_stream);
            }
            significance_flags.push_back(static_cast<uint8_t>(use_sparse_mode));
        }

        tables.push_back(std::move(selected_table));
        streams.push_back(std::move(selected_stream));
        if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
            sign_modes.push_back(streams.back().sign_mode);
        }
    }

    auto cluster_result = optimize_plane_table_clusters(blocks, spans, tables, streams, coding, config);
    if (!cluster_result) {
        return std::unexpected(cluster_result.error());
    }

    // Byte breakdown (Phase 1 diagnostic): measure each section as it is
    // written, so the caller can see whether tables or coded data dominate.
    size_t flag_bytes = 0;
    size_t table_bytes = 0;
    size_t stream_bytes = 0;
    if (config.use_significance_maps) {
        const size_t before = writer.size();
        auto flag_result = write_significance_mode_flags(writer, significance_flags);
        if (!flag_result) {
            return std::unexpected(flag_result.error());
        }
        flag_bytes += writer.size() - before;
    }
    if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
        const size_t before = writer.size();
        auto sign_result = write_sign_mode_flags(writer, sign_modes);
        if (!sign_result) {
            return std::unexpected(sign_result.error());
        }
        flag_bytes += writer.size() - before;
    }

    if (config.use_table_bank) {
        const size_t before = writer.size();
        auto bank_result = write_coefficient_table_bank(writer, tables, coding.num_symbols);
        if (!bank_result) {
            return std::unexpected(bank_result.error());
        }
        table_bytes += writer.size() - before;
    }

    const LossyCoeffTable* previous_table = nullptr;
    for (size_t context_index = 0; context_index < tables.size(); ++context_index) {
        if (!config.use_table_bank) {
            const size_t before = writer.size();
            auto table_result = write_lossy_coeff_table(writer, tables[context_index], coding.num_symbols, config,
                                                        previous_table);
            if (!table_result) {
                return std::unexpected(table_result.error());
            }
            table_bytes += writer.size() - before;
            previous_table = &tables[context_index];
        }
        const size_t before = writer.size();
        auto write_result = write_coeff_context_stream(writer, streams[context_index],
                                                       tables[context_index], coding, config);
        if (!write_result) {
            return std::unexpected(write_result.error());
        }
        stream_bytes += writer.size() - before;
    }

    if (stats != nullptr) {
        stats->flag_bytes = flag_bytes;
        stats->table_bytes = table_bytes;
        stats->stream_bytes = stream_bytes;
        stats->total_bytes = writer.size();
    }

    return writer.finish();
}

Result<std::vector<uint8_t>> encode_lossy_chroma_payload(
    std::span<const DctBlockI16> cb_blocks,
    std::span<const DctBlockI16> cr_blocks,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config,
    LossyCoeffPayloadStats* stats) {
    if (config.use_table_bank && !config.adaptive_coefficient_tables) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     "coefficient table bank requires adaptive coefficient tables"});
    }
    if (cb_blocks.size() != spans.size() || cr_blocks.size() != spans.size()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     "lossy chroma coefficient blocks and spans must match"});
    }

    const SymbolCoding coding = make_symbol_coding(config);
    const int coeff_limit = coefficient_limit(max_coeff_span, config);
    ByteWriter writer;
    std::vector<LossyCoeffTable> tables;
    std::vector<EncodedCoeffContextStream> cb_streams;
    std::vector<EncodedCoeffContextStream> cr_streams;
    std::vector<uint8_t> significance_flags;
    std::vector<uint8_t> cb_sign_modes;
    std::vector<uint8_t> cr_sign_modes;
    tables.reserve(static_cast<size_t>(coeff_limit));
    cb_streams.reserve(static_cast<size_t>(coeff_limit));
    cr_streams.reserve(static_cast<size_t>(coeff_limit));
    if (config.use_significance_maps) {
        significance_flags.reserve(static_cast<size_t>(coeff_limit));
    }
    if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
        cb_sign_modes.reserve(static_cast<size_t>(coeff_limit));
        cr_sign_modes.reserve(static_cast<size_t>(coeff_limit));
    }

    LossyCoeffStreamConfig raw_config = config;
    raw_config.use_significance_maps = false;
    LossyCoeffStreamConfig sparse_config = config;
    sparse_config.use_significance_maps = true;

    for (int coeff_index = 0; coeff_index < coeff_limit; ++coeff_index) {
        const CoeffContextAnalysis cb_analysis =
            analyze_coeff_context(cb_blocks, spans, coeff_index, config.use_significance_maps);
        const CoeffContextAnalysis cr_analysis =
            analyze_coeff_context(cr_blocks, spans, coeff_index, config.use_significance_maps);
        if (cb_analysis.active_blocks == 0) {
            tables.push_back(make_single_symbol_table(coding));
            EncodedCoeffContextStream empty_stream;
            cb_streams.push_back(empty_stream);
            cr_streams.push_back(std::move(empty_stream));
            if (config.use_significance_maps) {
                significance_flags.push_back(0);
            }
            if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
                cb_sign_modes.push_back(kCoefficientSignModeRawPacked);
                cr_sign_modes.push_back(kCoefficientSignModeRawPacked);
            }
            continue;
        }

        auto raw_counts = build_chroma_counts(cb_blocks, cr_blocks, spans, coeff_index, coding, false);
        LossyCoeffTable raw_table;
        raw_table.build_from_counts(raw_counts.data(), coding.num_symbols);
        auto raw_cb_stream = encode_coeff_context_stream(cb_blocks, spans, coeff_index, cb_analysis,
                                                         raw_table, coding, raw_config, false);
        if (!raw_cb_stream) {
            return std::unexpected(raw_cb_stream.error());
        }
        auto raw_cr_stream = encode_coeff_context_stream(cr_blocks, spans, coeff_index, cr_analysis,
                                                         raw_table, coding, raw_config, false);
        if (!raw_cr_stream) {
            return std::unexpected(raw_cr_stream.error());
        }

        bool use_sparse_mode = false;
        LossyCoeffTable selected_table = raw_table;
        EncodedCoeffContextStream selected_cb_stream = std::move(*raw_cb_stream);
        EncodedCoeffContextStream selected_cr_stream = std::move(*raw_cr_stream);

        const LossyCoeffTable* previous_table =
            (!config.use_table_bank && !tables.empty()) ? &tables.back() : nullptr;

        if (config.use_significance_maps) {
            auto sparse_counts = build_chroma_counts(cb_blocks, cr_blocks, spans, coeff_index, coding, true);
            LossyCoeffTable sparse_table;
            if (cb_analysis.nonzero_count + cr_analysis.nonzero_count == 0) {
                sparse_table = make_single_symbol_table(coding);
            } else {
                sparse_table.build_from_counts(sparse_counts.data(), coding.num_symbols);
            }
            auto sparse_cb_stream = encode_coeff_context_stream(cb_blocks, spans, coeff_index, cb_analysis,
                                                                sparse_table, coding, sparse_config, true);
            if (!sparse_cb_stream) {
                return std::unexpected(sparse_cb_stream.error());
            }
            auto sparse_cr_stream = encode_coeff_context_stream(cr_blocks, spans, coeff_index, cr_analysis,
                                                                sparse_table, coding, sparse_config, true);
            if (!sparse_cr_stream) {
                return std::unexpected(sparse_cr_stream.error());
            }

            auto raw_table_size = serialized_lossy_coeff_table_size(raw_table, coding.num_symbols, raw_config,
                                                                    previous_table);
            if (!raw_table_size) {
                return std::unexpected(raw_table_size.error());
            }
            auto raw_cb_stream_size = serialized_coeff_context_stream_size(*raw_cb_stream, raw_table, coding, raw_config);
            if (!raw_cb_stream_size) {
                return std::unexpected(raw_cb_stream_size.error());
            }
            auto raw_cr_stream_size = serialized_coeff_context_stream_size(*raw_cr_stream, raw_table, coding, raw_config);
            if (!raw_cr_stream_size) {
                return std::unexpected(raw_cr_stream_size.error());
            }
            auto sparse_table_size = serialized_lossy_coeff_table_size(sparse_table, coding.num_symbols, sparse_config,
                                                                       previous_table);
            if (!sparse_table_size) {
                return std::unexpected(sparse_table_size.error());
            }
            auto sparse_cb_stream_size = serialized_coeff_context_stream_size(*sparse_cb_stream, sparse_table, coding, sparse_config);
            if (!sparse_cb_stream_size) {
                return std::unexpected(sparse_cb_stream_size.error());
            }
            auto sparse_cr_stream_size = serialized_coeff_context_stream_size(*sparse_cr_stream, sparse_table, coding, sparse_config);
            if (!sparse_cr_stream_size) {
                return std::unexpected(sparse_cr_stream_size.error());
            }

            use_sparse_mode =
                *sparse_table_size + *sparse_cb_stream_size + *sparse_cr_stream_size <
                *raw_table_size + *raw_cb_stream_size + *raw_cr_stream_size;
            if (use_sparse_mode) {
                selected_table = std::move(sparse_table);
                selected_cb_stream = std::move(*sparse_cb_stream);
                selected_cr_stream = std::move(*sparse_cr_stream);
            }
            significance_flags.push_back(static_cast<uint8_t>(use_sparse_mode));
        }

        tables.push_back(std::move(selected_table));
        cb_streams.push_back(std::move(selected_cb_stream));
        cr_streams.push_back(std::move(selected_cr_stream));
        if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
            cb_sign_modes.push_back(cb_streams.back().sign_mode);
            cr_sign_modes.push_back(cr_streams.back().sign_mode);
        }
    }

    auto cluster_result = optimize_chroma_table_clusters(
        cb_blocks, cr_blocks, spans, tables, cb_streams, cr_streams, coding, config);
    if (!cluster_result) {
        return std::unexpected(cluster_result.error());
    }

    // Byte breakdown (Phase 1 diagnostic): see encode_lossy_plane_payload.
    size_t flag_bytes = 0;
    size_t table_bytes = 0;
    size_t stream_bytes = 0;
    if (config.use_significance_maps) {
        const size_t before = writer.size();
        auto flag_result = write_significance_mode_flags(writer, significance_flags);
        if (!flag_result) {
            return std::unexpected(flag_result.error());
        }
        flag_bytes += writer.size() - before;
    }
    if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
        const size_t before = writer.size();
        auto cb_sign_result = write_sign_mode_flags(writer, cb_sign_modes);
        if (!cb_sign_result) {
            return std::unexpected(cb_sign_result.error());
        }
        flag_bytes += writer.size() - before;
        const size_t before_cr = writer.size();
        auto cr_sign_result = write_sign_mode_flags(writer, cr_sign_modes);
        if (!cr_sign_result) {
            return std::unexpected(cr_sign_result.error());
        }
        flag_bytes += writer.size() - before_cr;
    }

    if (config.use_table_bank) {
        const size_t before = writer.size();
        auto bank_result = write_coefficient_table_bank(writer, tables, coding.num_symbols);
        if (!bank_result) {
            return std::unexpected(bank_result.error());
        }
        table_bytes += writer.size() - before;
    }

    const LossyCoeffTable* previous_table = nullptr;
    for (size_t context_index = 0; context_index < tables.size(); ++context_index) {
        if (!config.use_table_bank) {
            const size_t before = writer.size();
            auto table_result = write_lossy_coeff_table(writer, tables[context_index], coding.num_symbols, config,
                                                        previous_table);
            if (!table_result) {
                return std::unexpected(table_result.error());
            }
            table_bytes += writer.size() - before;
            previous_table = &tables[context_index];
        }
        const size_t before_cb = writer.size();
        auto write_cb = write_coeff_context_stream(writer, cb_streams[context_index],
                                                   tables[context_index], coding, config);
        if (!write_cb) {
            return std::unexpected(write_cb.error());
        }
        stream_bytes += writer.size() - before_cb;
        const size_t before_cr = writer.size();
        auto write_cr = write_coeff_context_stream(writer, cr_streams[context_index],
                                                   tables[context_index], coding, config);
        if (!write_cr) {
            return std::unexpected(write_cr.error());
        }
        stream_bytes += writer.size() - before_cr;
    }

    if (stats != nullptr) {
        stats->flag_bytes = flag_bytes;
        stats->table_bytes = table_bytes;
        stats->stream_bytes = stream_bytes;
        stats->total_bytes = writer.size();
    }

    return writer.finish();
}

Result<std::vector<DctBlockI16>> decode_lossy_plane_payload(
    ByteReader& reader,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config) {
    if (config.use_table_bank && !config.adaptive_coefficient_tables) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
                                     "coefficient table bank requires adaptive coefficient tables"});
    }
    const SymbolCoding coding = make_symbol_coding(config);
    const int coeff_limit = coefficient_limit(max_coeff_span, config);
    std::vector<DctBlockI16> blocks(spans.size());
    std::vector<uint8_t> significance_flags(static_cast<size_t>(coeff_limit), 0);
    std::vector<uint8_t> sign_modes(static_cast<size_t>(coeff_limit), kCoefficientSignModeRawPacked);

    if (config.use_significance_maps) {
        auto flags = read_significance_mode_flags(reader, static_cast<size_t>(coeff_limit));
        if (!flags) {
            return std::unexpected(flags.error());
        }
        significance_flags = std::move(*flags);
    }
    if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
        auto flags = read_sign_mode_flags(reader, static_cast<size_t>(coeff_limit));
        if (!flags) {
            return std::unexpected(flags.error());
        }
        sign_modes = std::move(*flags);
    }

    if (config.use_table_bank) {
        auto tables = read_coefficient_table_bank(reader, static_cast<size_t>(coeff_limit),
                                                  coding.num_symbols, "lossy");
        if (!tables) {
            return std::unexpected(tables.error());
        }
        for (int coeff_index = 0; coeff_index < coeff_limit; ++coeff_index) {
            auto stream = read_coeff_context_stream(reader, spans, coeff_index,
                                                    (*tables)[static_cast<size_t>(coeff_index)],
                                                    coding, blocks, "lossy", config,
                                                    significance_flags[static_cast<size_t>(coeff_index)] != 0,
                                                    sign_modes[static_cast<size_t>(coeff_index)]);
            if (!stream) {
                return std::unexpected(stream.error());
            }
        }
        return blocks;
    }

    std::optional<LossyCoeffTable> previous_table;
    for (int coeff_index = 0; coeff_index < coeff_limit; ++coeff_index) {
        auto table = read_lossy_coeff_table(reader, coding, config,
                                            previous_table ? &*previous_table : nullptr);
        if (!table) {
            return std::unexpected(table.error());
        }
        auto stream = read_coeff_context_stream(reader, spans, coeff_index, *table, coding,
                                                blocks, "lossy", config,
                                                significance_flags[static_cast<size_t>(coeff_index)] != 0,
                                                sign_modes[static_cast<size_t>(coeff_index)]);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        previous_table = *table;
    }

    return blocks;
}

Result<DecodedLossyChromaPayload> decode_lossy_chroma_payload(
    ByteReader& reader,
    std::span<const uint8_t> spans,
    uint8_t max_coeff_span,
    const LossyCoeffStreamConfig& config) {
    if (config.use_table_bank && !config.adaptive_coefficient_tables) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
                                     "coefficient table bank requires adaptive coefficient tables"});
    }
    const SymbolCoding coding = make_symbol_coding(config);
    const int coeff_limit = coefficient_limit(max_coeff_span, config);
    std::vector<uint8_t> significance_flags(static_cast<size_t>(coeff_limit), 0);
    std::vector<uint8_t> cb_sign_modes(static_cast<size_t>(coeff_limit), kCoefficientSignModeRawPacked);
    std::vector<uint8_t> cr_sign_modes(static_cast<size_t>(coeff_limit), kCoefficientSignModeRawPacked);

    if (config.use_significance_maps) {
        auto flags = read_significance_mode_flags(reader, static_cast<size_t>(coeff_limit));
        if (!flags) {
            return std::unexpected(flags.error());
        }
        significance_flags = std::move(*flags);
    }
    if (coding.split_magnitude_signs && config.use_adaptive_sign_streams) {
        auto cb_flags = read_sign_mode_flags(reader, static_cast<size_t>(coeff_limit));
        if (!cb_flags) {
            return std::unexpected(cb_flags.error());
        }
        cb_sign_modes = std::move(*cb_flags);

        auto cr_flags = read_sign_mode_flags(reader, static_cast<size_t>(coeff_limit));
        if (!cr_flags) {
            return std::unexpected(cr_flags.error());
        }
        cr_sign_modes = std::move(*cr_flags);
    }

    DecodedLossyChromaPayload payload;
    payload.cb_blocks.resize(spans.size());
    payload.cr_blocks.resize(spans.size());

    if (config.use_table_bank) {
        auto tables = read_coefficient_table_bank(reader, static_cast<size_t>(coeff_limit),
                                                  coding.num_symbols, "lossy");
        if (!tables) {
            return std::unexpected(tables.error());
        }
        for (int coeff_index = 0; coeff_index < coeff_limit; ++coeff_index) {
            const auto& table = (*tables)[static_cast<size_t>(coeff_index)];
            auto cb_stream = read_coeff_context_stream(reader, spans, coeff_index, table, coding,
                                                       payload.cb_blocks, "lossy", config,
                                                       significance_flags[static_cast<size_t>(coeff_index)] != 0,
                                                       cb_sign_modes[static_cast<size_t>(coeff_index)]);
            if (!cb_stream) {
                return std::unexpected(cb_stream.error());
            }

            auto cr_stream = read_coeff_context_stream(reader, spans, coeff_index, table, coding,
                                                       payload.cr_blocks, "lossy", config,
                                                       significance_flags[static_cast<size_t>(coeff_index)] != 0,
                                                       cr_sign_modes[static_cast<size_t>(coeff_index)]);
            if (!cr_stream) {
                return std::unexpected(cr_stream.error());
            }
        }
        return payload;
    }

    std::optional<LossyCoeffTable> previous_table;
    for (int coeff_index = 0; coeff_index < coeff_limit; ++coeff_index) {
        auto table = read_lossy_coeff_table(reader, coding, config,
                                            previous_table ? &*previous_table : nullptr);
        if (!table) {
            return std::unexpected(table.error());
        }

        auto cb_stream = read_coeff_context_stream(reader, spans, coeff_index, *table, coding,
                                                   payload.cb_blocks, "lossy", config,
                                                   significance_flags[static_cast<size_t>(coeff_index)] != 0,
                                                   cb_sign_modes[static_cast<size_t>(coeff_index)]);
        if (!cb_stream) {
            return std::unexpected(cb_stream.error());
        }

        auto cr_stream = read_coeff_context_stream(reader, spans, coeff_index, *table, coding,
                                                   payload.cr_blocks, "lossy", config,
                                                   significance_flags[static_cast<size_t>(coeff_index)] != 0,
                                                   cr_sign_modes[static_cast<size_t>(coeff_index)]);
        if (!cr_stream) {
            return std::unexpected(cr_stream.error());
        }

        previous_table = *table;
    }

    return payload;
}

}
