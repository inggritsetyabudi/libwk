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
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "../src/core/coeff_table_bank_stream.h"

using namespace wk;

namespace {

constexpr int kNumSymbols = 32;

LossyCoeffTable make_table(const std::initializer_list<std::pair<int, uint32_t>>& entries) {
    std::vector<uint32_t> counts(static_cast<size_t>(kNumSymbols), 0);
    for (const auto& [symbol, count] : entries) {
        counts[static_cast<size_t>(symbol)] = count;
    }

    LossyCoeffTable table;
    table.build_from_counts(counts.data(), kNumSymbols);
    return table;
}

void expect_tables_equal(std::span<const LossyCoeffTable> actual,
                         std::span<const LossyCoeffTable> expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t table_index = 0; table_index < expected.size(); ++table_index) {
        for (int symbol = 0; symbol < kNumSymbols; ++symbol) {
            EXPECT_EQ(actual[table_index].symbol(symbol).freq,
                      expected[table_index].symbol(symbol).freq)
                << "table " << table_index << " symbol " << symbol;
        }
    }
}

}

TEST(CoeffTableBankStreamTest, ChoosesInlineModeWhenItIsSmallest) {
    const std::array<LossyCoeffTable, 2> tables = {
        make_table({{1, 7}, {17, 5}, {31, 2}}),
        make_table({{8, 3}, {9, 5}, {10, 9}, {11, 4}})
    };

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::InlineTables));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, tables);
}

TEST(CoeffTableBankStreamTest, ChoosesSingleTableModeForRepeatedTables) {
    const std::array<LossyCoeffTable, 4> tables = {
        make_table({{3, 1}}),
        make_table({{3, 1}}),
        make_table({{3, 1}}),
        make_table({{3, 1}})
    };

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::SingleTable));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, tables);
}

TEST(CoeffTableBankStreamTest, ChoosesPackedBitModeForBinaryBanks) {
    const std::array<LossyCoeffTable, 8> tables = {
        make_table({{3, 1}}),
        make_table({{8, 3}, {9, 5}, {10, 9}, {11, 4}}),
        make_table({{3, 1}}),
        make_table({{8, 3}, {9, 5}, {10, 9}, {11, 4}}),
        make_table({{3, 1}}),
        make_table({{8, 3}, {9, 5}, {10, 9}, {11, 4}}),
        make_table({{3, 1}}),
        make_table({{8, 3}, {9, 5}, {10, 9}, {11, 4}})
    };

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::PackedBitIndices));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, tables);
}

TEST(CoeffTableBankStreamTest, ChoosesPackedTwoBitModeForFourEntryBanks) {
    const std::array<LossyCoeffTable, 12> tables = {
        make_table({{1, 1}}),
        make_table({{2, 1}}),
        make_table({{3, 1}}),
        make_table({{4, 1}}),
        make_table({{1, 1}}),
        make_table({{2, 1}}),
        make_table({{3, 1}}),
        make_table({{4, 1}}),
        make_table({{1, 1}}),
        make_table({{2, 1}}),
        make_table({{3, 1}}),
        make_table({{4, 1}})
    };

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::PackedTwoBitIndices));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, tables);
}

TEST(CoeffTableBankStreamTest, ChoosesPackedNibbleModeForSmallBanks) {
    std::vector<LossyCoeffTable> unique_tables;
    unique_tables.reserve(8);
    for (int index = 0; index < 8; ++index) {
        unique_tables.push_back(make_table({{index, 1}}));
    }

    std::vector<LossyCoeffTable> tables;
    tables.reserve(16);
    tables.insert(tables.end(), unique_tables.begin(), unique_tables.end());
    tables.insert(tables.end(), unique_tables.begin(), unique_tables.end());

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::PackedNibbleIndices));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, tables);
}

TEST(CoeffTableBankStreamTest, ChoosesRawByteModeForLargeBanks) {
    std::vector<LossyCoeffTable> unique_tables;
    unique_tables.reserve(17);
    for (int index = 0; index < 17; ++index) {
        unique_tables.push_back(make_table({{index, 1}}));
    }

    std::vector<LossyCoeffTable> tables;
    tables.reserve(34);
    tables.insert(tables.end(), unique_tables.begin(), unique_tables.end());
    tables.insert(tables.end(), unique_tables.begin(), unique_tables.end());

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::RawByteIndices));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, tables);
}

TEST(CoeffTableBankStreamTest, RejectsNonZeroPackedBitPadding) {
    ByteWriter writer;
    writer.write_u8(static_cast<uint8_t>(CoeffTableBankMode::PackedBitIndices));
    writer.write_u8(1);
    auto single = make_table({{3, 1}});
    auto table_result = write_coefficient_table(writer, single, kNumSymbols);
    ASSERT_TRUE(table_result.has_value()) << table_result.error().message;
    writer.write_u8(0xFE);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, 1, kNumSymbols, "unit");
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
}

TEST(CoeffTableBankStreamTest, RejectsNonZeroPackedNibblePadding) {
    ByteWriter writer;
    writer.write_u8(static_cast<uint8_t>(CoeffTableBankMode::PackedNibbleIndices));
    writer.write_u8(1);
    auto single = make_table({{3, 1}});
    auto table_result = write_coefficient_table(writer, single, kNumSymbols);
    ASSERT_TRUE(table_result.has_value()) << table_result.error().message;
    writer.write_u8(0xF0);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, 1, kNumSymbols, "unit");
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
}

TEST(CoeffTableBankStreamTest, RejectsOutOfRangeBankIndex) {
    ByteWriter writer;
    writer.write_u8(static_cast<uint8_t>(CoeffTableBankMode::RawByteIndices));
    writer.write_u8(1);
    auto single = make_table({{3, 1}});
    auto table_result = write_coefficient_table(writer, single, kNumSymbols);
    ASSERT_TRUE(table_result.has_value()) << table_result.error().message;
    writer.write_u8(1);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, 1, kNumSymbols, "unit");
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
}
