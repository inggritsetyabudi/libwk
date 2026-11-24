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

#include "../src/core/coeff_table_stream.h"

using namespace wk;

namespace {

LossyCoeffTable make_table(const std::initializer_list<std::pair<int, uint32_t>>& entries,
                           int num_symbols) {
    std::vector<uint32_t> counts(static_cast<size_t>(num_symbols), 0);
    for (const auto& [symbol, count] : entries) {
        counts[static_cast<size_t>(symbol)] = count;
    }

    LossyCoeffTable table;
    table.build_from_counts(counts.data(), num_symbols);
    return table;
}

void expect_tables_equal(const LossyCoeffTable& actual,
                         const LossyCoeffTable& expected,
                         int num_symbols) {
    for (int symbol = 0; symbol < num_symbols; ++symbol) {
        EXPECT_EQ(actual.symbol(symbol).freq, expected.symbol(symbol).freq)
            << "symbol " << symbol;
    }
}

}

TEST(CoeffTableStreamTest, SingleSymbolRoundTrip) {
    constexpr int kNumSymbols = 8;
    auto table = make_table({{3, 1}}, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::SingleSymbol));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 7, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(parsed->symbol(3).freq, LossyCoeffTable::TABLE_SIZE);
}

TEST(CoeffTableStreamTest, SparsePairsU8RoundTrip) {
    constexpr int kNumSymbols = 256;
    auto table = make_table({
        {0, 1}, {15, 1}, {30, 1}, {45, 1}, {60, 1}, {75, 1},
        {90, 1}, {105, 1}, {120, 1}, {135, 1}, {150, 1}, {165, 1},
        {180, 1}, {195, 1}, {210, 1}, {225, 1}, {240, 1}, {255, 1}
    }, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::SparsePairsU8));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 255, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    for (int i = 0; i < kNumSymbols; ++i) {
        EXPECT_EQ(parsed->symbol(i).freq, table.symbol(i).freq);
    }
}

TEST(CoeffTableStreamTest, DenseRangeU8RoundTrip) {
    constexpr int kNumSymbols = 64;
    auto table = make_table({
        {8, 1}, {9, 1}, {10, 1}, {11, 1}, {12, 1},
        {13, 1}, {14, 1}, {15, 1}, {16, 1}, {17, 1},
        {18, 1}, {19, 1}, {20, 1}, {21, 1}, {22, 1},
        {23, 1}, {24, 1}, {25, 1}, {26, 1}, {27, 1}
    }, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::DenseRangeU8));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 63, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    for (int i = 0; i < kNumSymbols; ++i) {
        EXPECT_EQ(parsed->symbol(i).freq, table.symbol(i).freq);
    }
}

TEST(CoeffTableStreamTest, FallsBackToDenseRangeU16ForWideFrequencies) {
    constexpr int kNumSymbols = 32;
    auto table = make_table({{8, 1}, {9, 1}}, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::DenseRange));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 31, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    for (int i = 0; i < kNumSymbols; ++i) {
        EXPECT_EQ(parsed->symbol(i).freq, table.symbol(i).freq);
    }
}

TEST(CoeffTableStreamTest, FallsBackToSparsePairsU16ForWideFrequencies) {
    constexpr int kNumSymbols = 32;
    auto table = make_table({{1, 1}, {17, 1}, {31, 1}}, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, kNumSymbols);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::SparsePairs));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 31, true);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    for (int i = 0; i < kNumSymbols; ++i) {
        EXPECT_EQ(parsed->symbol(i).freq, table.symbol(i).freq);
    }
}

TEST(CoeffTableStreamTest, ReusesPreviousTableWhenFrequenciesMatch) {
    constexpr int kNumSymbols = 32;
    auto table = make_table({{2, 1}, {7, 1}, {19, 1}}, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, table, kNumSymbols, &table);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::ReusePrevious));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 31, true, &table);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, table, kNumSymbols);
}

TEST(CoeffTableStreamTest, DeltaEncodesSmallSequentialTableChanges) {
    constexpr int kNumSymbols = 512;
    auto previous = make_table({{3, 1000}, {101, 1000}, {205, 1000}, {411, 1000}}, kNumSymbols);
    auto current = make_table({{3, 900}, {101, 1100}, {205, 1000}, {411, 1000}}, kNumSymbols);

    ByteWriter writer;
    auto written = write_coefficient_table(writer, current, kNumSymbols, &previous);
    ASSERT_TRUE(written.has_value()) << written.error().message;

    const auto bytes = writer.finish();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.front(), static_cast<uint8_t>(CoeffTableEncoding::DeltaSparseI8));

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, kNumSymbols, "unit", 511, true, &previous);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    expect_tables_equal(*parsed, current, kNumSymbols);
}

TEST(CoeffTableStreamTest, RejectsReuseWithoutPreviousTable) {
    ByteWriter writer;
    writer.write_u8(static_cast<uint8_t>(CoeffTableEncoding::ReusePrevious));
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, 8, "unit", 7, true);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
    EXPECT_NE(parsed.error().message.find("missing previous table"), std::string::npos);
}

TEST(CoeffTableStreamTest, RejectsDeltaWithoutPreviousTable) {
    ByteWriter writer;
    writer.write_u8(static_cast<uint8_t>(CoeffTableEncoding::DeltaSparseI8));
    writer.write_u16(1);
    writer.write_u16(7);
    writer.write_u8(1);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, 32, "unit", 31, true);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
    EXPECT_NE(parsed.error().message.find("without a previous table"), std::string::npos);
}

TEST(CoeffTableStreamTest, RejectsUnknownEncoding) {
    ByteWriter writer;
    writer.write_u8(0xFF);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto parsed = read_coefficient_table(reader, 8, "unit", 7, true);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, ErrorCode::RansError);
}
