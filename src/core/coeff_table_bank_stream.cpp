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
#include "coeff_table_bank_stream.h"

namespace wk {

namespace {

struct TableBankEntry {
    std::vector<uint8_t> serialized;
    LossyCoeffTable table;
};

[[nodiscard]] Error invalid_bank_error(std::string_view label, std::string message) {
    return Error{
        ErrorCode::RansError,
        std::string(label) + " coefficient table bank " + std::move(message),
    };
}

[[nodiscard]] Result<std::vector<uint8_t>> serialize_table(const LossyCoeffTable& table,
                                                           int num_symbols) {
    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, num_symbols);
    if (!written) {
        return std::unexpected(written.error());
    }
    return writer.finish();
}

[[nodiscard]] size_t packed_index_bytes(size_t index_count, uint8_t bits_per_index) {
    return (index_count * bits_per_index + 7u) / 8u;
}

[[nodiscard]] Result<std::vector<uint8_t>> pack_fixed_width_indices(std::span<const uint8_t> indices,
                                                                    uint8_t bits_per_index) {
    if (bits_per_index != 1u && bits_per_index != 2u && bits_per_index != 4u) {
        return std::unexpected(Error{
            ErrorCode::InvalidParameter,
            "coefficient table bank packed index width is unsupported",
        });
    }

    const uint8_t max_index = static_cast<uint8_t>((1u << bits_per_index) - 1u);
    std::vector<uint8_t> bytes(packed_index_bytes(indices.size(), bits_per_index), 0);
    size_t bit_offset = 0;
    for (size_t index = 0; index < indices.size(); ++index) {
        if (indices[index] > max_index) {
            return std::unexpected(
                Error{ErrorCode::InvalidParameter, "coefficient table bank packed index is out of range"});
        }
        const size_t byte_index = bit_offset / 8u;
        const uint8_t bit_shift = static_cast<uint8_t>(bit_offset % 8u);
        bytes[byte_index] |= static_cast<uint8_t>(indices[index] << bit_shift);
        if (bit_shift + bits_per_index > 8u) {
            bytes[byte_index + 1] |= static_cast<uint8_t>(indices[index] >> (8u - bit_shift));
        }
        bit_offset += bits_per_index;
    }
    return bytes;
}

[[nodiscard]] Result<std::vector<uint8_t>> unpack_fixed_width_indices(std::span<const uint8_t> bytes,
                                                                      size_t expected_count,
                                                                      uint8_t bits_per_index,
                                                                      std::string_view label) {
    if (bits_per_index != 1u && bits_per_index != 2u && bits_per_index != 4u) {
        return std::unexpected(invalid_bank_error(label, "uses an unsupported packed index width"));
    }

    const size_t expected_bytes = packed_index_bytes(expected_count, bits_per_index);
    if (bytes.size() != expected_bytes) {
        return std::unexpected(invalid_bank_error(label, "packed index stream size mismatch"));
    }

    std::vector<uint8_t> indices(expected_count, 0);
    const uint8_t mask = static_cast<uint8_t>((1u << bits_per_index) - 1u);
    for (size_t index = 0; index < expected_count; ++index) {
        const size_t bit_offset = index * bits_per_index;
        const size_t byte_index = bit_offset / 8u;
        const uint8_t bit_shift = static_cast<uint8_t>(bit_offset % 8u);
        uint16_t value = static_cast<uint16_t>(bytes[byte_index] >> bit_shift);
        if (bit_shift + bits_per_index > 8u) {
            value |= static_cast<uint16_t>(bytes[byte_index + 1]) << (8u - bit_shift);
        }
        indices[index] = static_cast<uint8_t>(value & mask);
    }

    const size_t used_bits = expected_count * bits_per_index;
    if (!bytes.empty() && (used_bits % 8u) != 0u) {
        const uint8_t used_low_bits = static_cast<uint8_t>(used_bits % 8u);
        const uint8_t padding_mask = static_cast<uint8_t>(0xFFu << used_low_bits);
        if ((bytes.back() & padding_mask) != 0) {
            return std::unexpected(invalid_bank_error(label, "has non-zero packed-index padding"));
        }
    }

    return indices;
}

[[nodiscard]] Result<std::vector<TableBankEntry>> build_table_bank(std::span<const LossyCoeffTable> tables,
                                                                   int num_symbols,
                                                                   std::vector<uint8_t>& table_indices,
                                                                   size_t& inline_size) {
    std::vector<TableBankEntry> bank;
    table_indices.clear();
    table_indices.reserve(tables.size());
    inline_size = 1u;

    for (const auto& table : tables) {
        auto serialized = serialize_table(table, num_symbols);
        if (!serialized) {
            return std::unexpected(serialized.error());
        }
        inline_size += serialized->size();

        size_t bank_index = 0;
        while (bank_index < bank.size() && bank[bank_index].serialized != *serialized) {
            ++bank_index;
        }
        if (bank_index == bank.size()) {
            bank.push_back(TableBankEntry{
                .serialized = std::move(*serialized),
                .table = table,
            });
        }
        table_indices.push_back(static_cast<uint8_t>(bank_index));
    }

    return bank;
}

[[nodiscard]] Result<void> write_inline_tables(ByteWriter& writer,
                                               std::span<const LossyCoeffTable> tables,
                                               int num_symbols) {
    writer.write_u8(static_cast<uint8_t>(CoeffTableBankMode::InlineTables));
    for (const auto& table : tables) {
        auto written = write_coefficient_table(writer, table, num_symbols);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    return {};
}

[[nodiscard]] Result<void> write_bank_tables(ByteWriter& writer,
                                             std::span<const TableBankEntry> bank,
                                             CoeffTableBankMode mode,
                                             std::span<const uint8_t> indices) {
    writer.write_u8(static_cast<uint8_t>(mode));
    if (mode == CoeffTableBankMode::SingleTable) {
        writer.write_bytes(bank.front().serialized);
        return {};
    }

    writer.write_u8(static_cast<uint8_t>(bank.size()));
    for (const auto& entry : bank) {
        writer.write_bytes(entry.serialized);
    }

    if (mode == CoeffTableBankMode::PackedBitIndices ||
        mode == CoeffTableBankMode::PackedTwoBitIndices ||
        mode == CoeffTableBankMode::PackedNibbleIndices) {
        const uint8_t bits_per_index =
            mode == CoeffTableBankMode::PackedBitIndices ? 1u :
            mode == CoeffTableBankMode::PackedTwoBitIndices ? 2u : 4u;
        auto packed = pack_fixed_width_indices(indices, bits_per_index);
        if (!packed) {
            return std::unexpected(packed.error());
        }
        writer.write_bytes(*packed);
        return {};
    }

    writer.write_bytes(indices);
    return {};
}

[[nodiscard]] Result<std::vector<TableBankEntry>> read_bank_entries(ByteReader& reader,
                                                                    uint8_t bank_size,
                                                                    int num_symbols,
                                                                    std::string_view label) {
    if (bank_size == 0) {
        return std::unexpected(invalid_bank_error(label, "declares zero bank entries"));
    }

    std::vector<TableBankEntry> bank;
    bank.reserve(bank_size);
    for (uint8_t bank_index = 0; bank_index < bank_size; ++bank_index) {
        auto table = read_coefficient_table(reader, num_symbols, label);
        if (!table) {
            return std::unexpected(table.error());
        }
        auto serialized = serialize_table(*table, num_symbols);
        if (!serialized) {
            return std::unexpected(serialized.error());
        }
        bank.push_back(TableBankEntry{
            .serialized = std::move(*serialized),
            .table = std::move(*table),
        });
    }
    return bank;
}

[[nodiscard]] Result<std::vector<LossyCoeffTable>> materialize_bank_tables(std::span<const TableBankEntry> bank,
                                                                           std::span<const uint8_t> indices,
                                                                           std::string_view label) {
    std::vector<LossyCoeffTable> tables;
    tables.reserve(indices.size());
    for (uint8_t bank_index : indices) {
        if (bank_index >= bank.size()) {
            return std::unexpected(invalid_bank_error(label, "references an out-of-range bank entry"));
        }
        tables.push_back(bank[bank_index].table);
    }
    return tables;
}

}

Result<void> write_coefficient_table_bank(ByteWriter& writer,
                                          std::span<const LossyCoeffTable> tables,
                                          int num_symbols) {
    if (tables.empty()) {
        return {};
    }

    std::vector<uint8_t> table_indices;
    size_t inline_size = 0;
    auto bank = build_table_bank(tables, num_symbols, table_indices, inline_size);
    if (!bank) {
        return std::unexpected(bank.error());
    }

    CoeffTableBankMode best_mode = CoeffTableBankMode::InlineTables;
    size_t best_size = inline_size;

    if (bank->size() == 1) {
        const size_t single_size = 1u + bank->front().serialized.size();
        if (single_size < best_size) {
            best_mode = CoeffTableBankMode::SingleTable;
            best_size = single_size;
        }
    }

    size_t bank_payload_size = 2u;
    for (const auto& entry : *bank) {
        bank_payload_size += entry.serialized.size();
    }

    if (bank->size() <= 2u) {
        const size_t bit_size = bank_payload_size + packed_index_bytes(table_indices.size(), 1u);
        if (bit_size < best_size) {
            best_mode = CoeffTableBankMode::PackedBitIndices;
            best_size = bit_size;
        }
    }

    if (bank->size() <= 4u) {
        const size_t two_bit_size = bank_payload_size + packed_index_bytes(table_indices.size(), 2u);
        if (two_bit_size < best_size) {
            best_mode = CoeffTableBankMode::PackedTwoBitIndices;
            best_size = two_bit_size;
        }
    }

    if (bank->size() <= 16u) {
        const size_t nibble_size = bank_payload_size + packed_index_bytes(table_indices.size(), 4u);
        if (nibble_size < best_size) {
            best_mode = CoeffTableBankMode::PackedNibbleIndices;
            best_size = nibble_size;
        }
    }

    if (bank->size() <= 255u) {
        const size_t byte_size = bank_payload_size + table_indices.size();
        if (byte_size < best_size) {
            best_mode = CoeffTableBankMode::RawByteIndices;
            best_size = byte_size;
        }
    }

    if (best_mode == CoeffTableBankMode::InlineTables) {
        return write_inline_tables(writer, tables, num_symbols);
    }
    return write_bank_tables(writer, *bank, best_mode, table_indices);
}

Result<std::vector<LossyCoeffTable>> read_coefficient_table_bank(ByteReader& reader,
                                                                 size_t expected_count,
                                                                 int num_symbols,
                                                                 std::string_view label) {
    if (expected_count == 0) {
        return std::vector<LossyCoeffTable>{};
    }

    auto mode_read = reader.read_u8();
    if (!mode_read) {
        return std::unexpected(mode_read.error());
    }

    switch (static_cast<CoeffTableBankMode>(*mode_read)) {
    case CoeffTableBankMode::InlineTables: {
        std::vector<LossyCoeffTable> tables;
        tables.reserve(expected_count);
        std::optional<LossyCoeffTable> previous_table;
        for (size_t index = 0; index < expected_count; ++index) {
            auto table = read_coefficient_table(reader, num_symbols, label,
                                                previous_table ? &*previous_table : nullptr);
            if (!table) {
                return std::unexpected(table.error());
            }
            previous_table = *table;
            tables.push_back(std::move(*table));
        }
        return tables;
    }
    case CoeffTableBankMode::SingleTable: {
        auto table = read_coefficient_table(reader, num_symbols, label);
        if (!table) {
            return std::unexpected(table.error());
        }
        return std::vector<LossyCoeffTable>(expected_count, *table);
    }
    case CoeffTableBankMode::PackedNibbleIndices: {
        auto bank_size = reader.read_u8();
        if (!bank_size) {
            return std::unexpected(bank_size.error());
        }
        if (*bank_size > 16u) {
            return std::unexpected(invalid_bank_error(label, "has too many nibble bank entries"));
        }
        auto bank = read_bank_entries(reader, *bank_size, num_symbols, label);
        if (!bank) {
            return std::unexpected(bank.error());
        }
        const size_t index_bytes = packed_index_bytes(expected_count, 4u);
        auto packed_indices = reader.read_bytes(index_bytes);
        if (!packed_indices) {
            return std::unexpected(packed_indices.error());
        }
        auto indices = unpack_fixed_width_indices(*packed_indices, expected_count, 4u, label);
        if (!indices) {
            return std::unexpected(indices.error());
        }
        return materialize_bank_tables(*bank, *indices, label);
    }
    case CoeffTableBankMode::PackedBitIndices:
    case CoeffTableBankMode::PackedTwoBitIndices: {
        auto bank_size = reader.read_u8();
        if (!bank_size) {
            return std::unexpected(bank_size.error());
        }
        const uint8_t max_bank_size =
            *mode_read == static_cast<uint8_t>(CoeffTableBankMode::PackedBitIndices) ? 2u : 4u;
        if (*bank_size > max_bank_size) {
            return std::unexpected(invalid_bank_error(label, "has too many packed bank entries"));
        }
        auto bank = read_bank_entries(reader, *bank_size, num_symbols, label);
        if (!bank) {
            return std::unexpected(bank.error());
        }
        const uint8_t bits_per_index =
            *mode_read == static_cast<uint8_t>(CoeffTableBankMode::PackedBitIndices) ? 1u : 2u;
        const size_t index_bytes = packed_index_bytes(expected_count, bits_per_index);
        auto packed_indices = reader.read_bytes(index_bytes);
        if (!packed_indices) {
            return std::unexpected(packed_indices.error());
        }
        auto indices = unpack_fixed_width_indices(*packed_indices, expected_count, bits_per_index, label);
        if (!indices) {
            return std::unexpected(indices.error());
        }
        return materialize_bank_tables(*bank, *indices, label);
    }
    case CoeffTableBankMode::RawByteIndices: {
        auto bank_size = reader.read_u8();
        if (!bank_size) {
            return std::unexpected(bank_size.error());
        }
        auto bank = read_bank_entries(reader, *bank_size, num_symbols, label);
        if (!bank) {
            return std::unexpected(bank.error());
        }
        auto indices = reader.read_bytes(expected_count);
        if (!indices) {
            return std::unexpected(indices.error());
        }
        return materialize_bank_tables(*bank, *indices, label);
    }
    default:
        return std::unexpected(invalid_bank_error(label, "uses an unknown encoding"));
    }
}

}
