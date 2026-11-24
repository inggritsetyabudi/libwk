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
#include <optional>
#include <vector>

#include "../src/core/coeff_table_bank_stream.h"
#include "../src/core/parametric_table.h"

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
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, 1, kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, 1, kNumSymbols, "unit", 31, true);
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
    auto parsed = read_coefficient_table_bank(reader, 1, kNumSymbols, "unit", 31, true);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
}

namespace {

// Counts for a wide magnitude distribution (split, limit 31):
// zeros dominate, ones frequent, geometric-like tail out to m=20.
std::vector<uint32_t> wide_counts() {
    std::vector<uint32_t> counts(static_cast<size_t>(kNumSymbols), 0);
    counts[0] = 60;
    counts[1] = 25;
    counts[2] = 8;
    counts[3] = 4;
    counts[4] = 2;
    counts[5] = 1;
    counts[10] = 3;
    counts[15] = 2;
    counts[20] = 1;
    return counts;
}

std::vector<uint32_t> wide_counts_shifted() {
    std::vector<uint32_t> counts(static_cast<size_t>(kNumSymbols), 0);
    counts[0] = 55;
    counts[1] = 30;
    counts[2] = 6;
    counts[3] = 5;
    counts[4] = 3;
    counts[6] = 2;
    counts[12] = 2;
    counts[18] = 1;
    counts[25] = 1;
    return counts;
}

LossyCoeffTable table_from_counts(const std::vector<uint32_t>& counts) {
    LossyCoeffTable table;
    table.build_from_counts(counts.data(), kNumSymbols);
    return table;
}

}  // namespace

TEST(CoeffTableBankStreamTest, ParametricEntriesRoundTrip) {
    const auto counts0 = wide_counts();
    const auto counts2 = wide_counts_shifted();
    const auto params0 = fit_parametric_m3(counts0, /*limit=*/31, /*split=*/true);
    const auto params2 = fit_parametric_m3(counts2, /*limit=*/31, /*split=*/true);
    ASSERT_TRUE(params0.has_value());
    ASSERT_TRUE(params2.has_value());

    const std::array<LossyCoeffTable, 3> tables = {
        table_from_counts(counts0),
        make_table({{3, 5}, {9, 3}, {17, 2}}),
        table_from_counts(counts2),
    };
    const std::array<std::optional<ParametricM3>, 3> params = {
        params0, std::nullopt, params2};

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols, params);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();

    // The same bank without parametric entries is larger: each wide
    // dense-range table costs ~47 B, each parametric entry only 8 B.
    ByteWriter plain_writer;
    ASSERT_TRUE(write_coefficient_table_bank(plain_writer, tables, kNumSymbols).has_value());
    EXPECT_LT(bytes.size(), plain_writer.finish().size());

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    const std::array<LossyCoeffTable, 3> expected = {
        reconstruct_parametric_m3(*params0, /*limit=*/31, /*split=*/true),
        tables[1],
        reconstruct_parametric_m3(*params2, /*limit=*/31, /*split=*/true),
    };
    expect_tables_equal(*parsed, expected);
}

TEST(CoeffTableBankStreamTest, ParametricEntriesUseInlineModeWhenSmallest) {
    const auto counts0 = wide_counts();
    const auto counts1 = wide_counts_shifted();
    const auto params0 = fit_parametric_m3(counts0, /*limit=*/31, /*split=*/true);
    const auto params1 = fit_parametric_m3(counts1, /*limit=*/31, /*split=*/true);
    ASSERT_TRUE(params0.has_value());
    ASSERT_TRUE(params1.has_value());

    const std::array<LossyCoeffTable, 2> tables = {
        table_from_counts(counts0),
        table_from_counts(counts1),
    };
    const std::array<std::optional<ParametricM3>, 2> params = {params0, params1};

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols, params);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableBankMode::InlineTables));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    const std::array<LossyCoeffTable, 2> expected = {
        reconstruct_parametric_m3(*params0, /*limit=*/31, /*split=*/true),
        reconstruct_parametric_m3(*params1, /*limit=*/31, /*split=*/true),
    };
    expect_tables_equal(*parsed, expected);
}

TEST(CoeffTableBankStreamTest, ParametricEntriesShareOneBankEntryWhenParamsMatch) {
    const auto counts0 = wide_counts();
    const auto counts2 = wide_counts_shifted();
    const auto params0 = fit_parametric_m3(counts0, /*limit=*/31, /*split=*/true);
    const auto params2 = fit_parametric_m3(counts2, /*limit=*/31, /*split=*/true);
    ASSERT_TRUE(params0.has_value());
    ASSERT_TRUE(params2.has_value());

    const std::array<LossyCoeffTable, 4> tables = {
        table_from_counts(counts0),
        table_from_counts(counts0),
        table_from_counts(counts2),
        make_table({{3, 5}, {9, 3}, {17, 2}}),
    };
    const std::array<std::optional<ParametricM3>, 4> params = {
        params0, params0, params2, std::nullopt};

    ByteWriter writer;
    auto written = write_coefficient_table_bank(writer, tables, kNumSymbols, params);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();

    ByteWriter plain_writer;
    ASSERT_TRUE(write_coefficient_table_bank(plain_writer, tables, kNumSymbols).has_value());
    EXPECT_LT(bytes.size(), plain_writer.finish().size());

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table_bank(reader, tables.size(), kNumSymbols, "unit", 31, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    const auto fitted0 = reconstruct_parametric_m3(*params0, /*limit=*/31, /*split=*/true);
    const auto fitted2 = reconstruct_parametric_m3(*params2, /*limit=*/31, /*split=*/true);
    const std::array<LossyCoeffTable, 4> expected = {fitted0, fitted0, fitted2, tables[3]};
    expect_tables_equal(*parsed, expected);
}
