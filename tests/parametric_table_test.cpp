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
#include <cstdint>
#include <vector>

#include "../src/core/coeff_table_stream.h"
#include "../src/core/parametric_table.h"

using namespace wk;

namespace {

// Magnitude counts for the hand-computed split example:
// m=0:60, m=1:25, m=2:8, m=3:4, m=4:2, m=5:1 (n=100).
// z=0.60 -> z_q=2458, p1=0.25 -> p1_q=1024, tail mean 26/15 -> r_q=108,
// rest=614, mag = [2458, 1024, 354, 150, 63, 27, 11, 5, 2, 1, 0, ...].
std::vector<uint32_t> split_counts_example() {
    std::vector<uint32_t> counts(17, 0);
    counts[0] = 60;
    counts[1] = 25;
    counts[2] = 8;
    counts[3] = 4;
    counts[4] = 2;
    counts[5] = 1;
    return counts;
}

std::vector<uint32_t> expected_split_counts() {
    return {2458, 1024, 354, 150, 63, 27, 11, 5, 2, 1, 0, 0, 0, 0, 0, 0, 0};
}

void expect_table_matches_counts(const LossyCoeffTable& table,
                                 const std::vector<uint32_t>& counts) {
    LossyCoeffTable expected;
    expected.build_from_counts(counts.data(), static_cast<int>(counts.size()));
    ASSERT_EQ(table.num_symbols(), expected.num_symbols());
    for (int symbol = 0; symbol < expected.num_symbols(); ++symbol) {
        EXPECT_EQ(table.symbol(symbol).freq, expected.symbol(symbol).freq)
            << "symbol " << symbol;
    }
}

}  // namespace

TEST(ParametricTableTest, FitMatchesHandComputedSplitDistribution) {
    const auto counts = split_counts_example();
    auto params = fit_parametric_m3(counts, /*limit=*/16, /*split=*/true);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->z_q, 2458);
    EXPECT_EQ(params->p1_q, 1024);
    EXPECT_EQ(params->r_q, 108);
    EXPECT_EQ(params->max_mag, 5);
}

TEST(ParametricTableTest, FitMatchesHandComputedNonSplitDistribution) {
    // Same magnitudes as the split example, stored as signed symbols
    // around offset 16: s=16 -> 60, s=15/17 -> 12/13, s=14/18 -> 4/4,
    // s=13/19 -> 2/2, s=12/20 -> 1/1, s=11 -> 1 (n=100, max_mag=5).
    std::vector<uint32_t> counts(33, 0);
    counts[16] = 60;
    counts[15] = 12;
    counts[17] = 13;
    counts[14] = 4;
    counts[18] = 4;
    counts[13] = 2;
    counts[19] = 2;
    counts[12] = 1;
    counts[20] = 1;
    counts[11] = 1;

    auto params = fit_parametric_m3(counts, /*limit=*/16, /*split=*/false);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->z_q, 2458);
    EXPECT_EQ(params->p1_q, 1024);
    EXPECT_EQ(params->r_q, 108);
    EXPECT_EQ(params->max_mag, 5);
}

TEST(ParametricTableTest, ReconstructMatchesExpectedSplitCounts) {
    const ParametricM3 params{.z_q = 2458, .p1_q = 1024, .r_q = 108, .max_mag = 5};
    const auto table = reconstruct_parametric_m3(params, /*limit=*/16, /*split=*/true);
    EXPECT_EQ(table.num_symbols(), 17);
    expect_table_matches_counts(table, expected_split_counts());
}

TEST(ParametricTableTest, ReconstructMatchesExpectedNonSplitCounts) {
    // Same params as the split example; non-split halves the fitted
    // magnitude doubles symmetrically: mag 26.89/2 -> 13, 11.39/2 -> 6,
    // 4.82/2 -> 2, 2.04/2 -> 1, 0.87/2 -> 0.
    const ParametricM3 params{.z_q = 2458, .p1_q = 1024, .r_q = 108, .max_mag = 4};
    const auto table = reconstruct_parametric_m3(params, /*limit=*/16, /*split=*/false);
    EXPECT_EQ(table.num_symbols(), 33);
    const std::vector<uint32_t> expected = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 6, 13, 32, 75, 177, 512, 2458,
        512, 177, 75, 32, 13, 6, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    expect_table_matches_counts(table, expected);
}

TEST(ParametricTableTest, ReconstructFloorsLargeMagnitudesWithinMaxMag) {
    // Sparse tail: m=0:80, m=1:15, m=2:4, m=50:1 (n=100, limit 64).
    // z_q=3277, p1_q=614, r_q=231, rest=205; the geometric tail at m=50
    // rounds to 0, so only the max_mag floor keeps symbol 50 covered.
    std::vector<uint32_t> counts(65, 0);
    counts[0] = 80;
    counts[1] = 15;
    counts[2] = 4;
    counts[50] = 1;

    auto params = fit_parametric_m3(counts, /*limit=*/64, /*split=*/true);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->z_q, 3277);
    EXPECT_EQ(params->p1_q, 614);
    EXPECT_EQ(params->r_q, 231);
    EXPECT_EQ(params->max_mag, 50);

    const auto table = reconstruct_parametric_m3(*params, /*limit=*/64, /*split=*/true);
    EXPECT_GT(table.symbol(50).freq, 0) << "m=50 must be floored to >= 1";
    EXPECT_EQ(table.symbol(51).freq, 0) << "beyond max_mag the tail stays 0";
    EXPECT_GT(table.symbol(0).freq, table.symbol(50).freq);
}

TEST(ParametricTableTest, ReconstructAllZerosIsSingleSymbol) {
    std::vector<uint32_t> counts(17, 0);
    counts[0] = 100;

    auto params = fit_parametric_m3(counts, /*limit=*/16, /*split=*/true);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->z_q, 4096);
    EXPECT_EQ(params->p1_q, 0);
    EXPECT_EQ(params->r_q, 128);
    EXPECT_EQ(params->max_mag, 0);

    const auto table = reconstruct_parametric_m3(*params, /*limit=*/16, /*split=*/true);
    EXPECT_EQ(table.symbol(0).freq, LossyCoeffTable::TABLE_SIZE);
    EXPECT_EQ(table.symbol(1).freq, 0);
}

TEST(ParametricTableTest, FitReturnsNulloptForEmptyCounts) {
    const std::vector<uint32_t> counts(17, 0);
    EXPECT_FALSE(fit_parametric_m3(counts, /*limit=*/16, /*split=*/true).has_value());
    EXPECT_FALSE(fit_parametric_m3(counts, /*limit=*/16, /*split=*/false).has_value());
}

TEST(ParametricTableTest, FitClampsQuantizerRoundingCorner) {
    // n=8192, n0=4097, n1=4095: z*4096 rounds to 2049 and p1*4096 to
    // 2048, summing to 4097, so the fit must clamp p1_q down by 1.
    std::vector<uint32_t> counts(17, 0);
    counts[0] = 4097;
    counts[1] = 4095;

    auto params = fit_parametric_m3(counts, /*limit=*/16, /*split=*/true);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->z_q, 2049);
    EXPECT_EQ(params->p1_q, 2047);
    EXPECT_EQ(params->r_q, 128);
    EXPECT_EQ(params->max_mag, 1);

    const auto table = reconstruct_parametric_m3(*params, /*limit=*/16, /*split=*/true);
    EXPECT_GT(table.symbol(0).freq, 0);
    EXPECT_GT(table.symbol(1).freq, 0);
    EXPECT_EQ(table.symbol(2).freq, 0);
}

TEST(ParametricTableTest, ReconstructEdgeRValuesAreDeterministic) {
    // r_q=0: tail is rest at m=2 then 0; r_q=255: tail is 0 everywhere.
    // In both cases the max_mag floor keeps m<=4 covered and m>4 empty.
    for (const uint8_t r_q : {static_cast<uint8_t>(0), static_cast<uint8_t>(255)}) {
        const ParametricM3 params{.z_q = 2048, .p1_q = 1024, .r_q = r_q, .max_mag = 4};
        const auto first = reconstruct_parametric_m3(params, /*limit=*/16, /*split=*/true);
        const auto second = reconstruct_parametric_m3(params, /*limit=*/16, /*split=*/true);
        for (int symbol = 0; symbol < 17; ++symbol) {
            EXPECT_EQ(first.symbol(symbol).freq, second.symbol(symbol).freq)
                << "r_q=" << static_cast<int>(r_q) << " symbol " << symbol;
        }
        EXPECT_GT(first.symbol(3).freq, 0);
        EXPECT_GT(first.symbol(4).freq, 0);
        EXPECT_EQ(first.symbol(5).freq, 0);
    }
}

TEST(ParametricTableTest, ReconstructCapsFlooredSymbolsAtTableSize) {
    // 12-bit-scale limit with a huge max_mag: flooring every magnitude
    // would need more nonzero symbols than the rANS table holds, so the
    // floor stops at TABLE_SIZE and far magnitudes stay zero instead of
    // overflowing the table normalization.
    const ParametricM3 params{.z_q = 2048, .p1_q = 1024, .r_q = 128, .max_mag = 5000};
    const auto table = reconstruct_parametric_m3(params, /*limit=*/8192, /*split=*/true);
    EXPECT_EQ(table.num_symbols(), 8193);
    uint32_t total = 0;
    uint32_t nonzero = 0;
    for (int symbol = 0; symbol < table.num_symbols(); ++symbol) {
        total += table.symbol(symbol).freq;
        if (table.symbol(symbol).freq > 0) {
            ++nonzero;
        }
    }
    EXPECT_EQ(total, LossyCoeffTable::TABLE_SIZE);
    EXPECT_LE(nonzero, LossyCoeffTable::TABLE_SIZE);
    EXPECT_GT(table.symbol(4000).freq, 0);
    EXPECT_EQ(table.symbol(5000).freq, 0);
}

TEST(ParametricTableTest, WriteReadParamEntryRoundTrip) {
    const ParametricM3 params{.z_q = 2458, .p1_q = 1024, .r_q = 108, .max_mag = 5};

    ByteWriter writer;
    write_parametric_m3_entry(writer, params);
    const auto bytes = writer.finish();
    ASSERT_EQ(bytes.size(), 8u);
    // Tag 7, then z u16 LE, p1 u16 LE, r u8, max_mag u16 LE.
    const std::vector<uint8_t> expected = {7, 0x9A, 0x09, 0x00, 0x04, 0x6C, 0x05, 0x00};
    EXPECT_EQ(bytes, expected);

    ByteReader reader(bytes);
    auto tag = reader.read_u8();
    ASSERT_TRUE(tag.has_value());
    EXPECT_EQ(*tag, 7);
    auto parsed = read_parametric_m3_params(reader, /*limit=*/16, "unit");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_EQ(*parsed, params);
}

TEST(ParametricTableTest, ReadParamEntryRejectsOutOfRangeParams) {
    // Params only (7 bytes, no tag): the caller consumes the tag byte
    // before calling read_parametric_m3_params.
    auto make_bytes = [](uint16_t z_q, uint16_t p1_q, uint8_t r_q, uint16_t max_mag) {
        ByteWriter writer;
        writer.write_u16(z_q);
        writer.write_u16(p1_q);
        writer.write_u8(r_q);
        writer.write_u16(max_mag);
        return writer.finish();
    };

    // z_q above the 4096 ceiling.
    {
        ByteReader reader(make_bytes(4097, 0, 128, 0));
        EXPECT_FALSE(read_parametric_m3_params(reader, /*limit=*/16, "unit").has_value());
    }
    // p1_q above the 4096 ceiling.
    {
        ByteReader reader(make_bytes(0, 4097, 128, 0));
        EXPECT_FALSE(read_parametric_m3_params(reader, /*limit=*/16, "unit").has_value());
    }
    // z_q + p1_q above 4096.
    {
        ByteReader reader(make_bytes(3000, 2000, 128, 0));
        EXPECT_FALSE(read_parametric_m3_params(reader, /*limit=*/16, "unit").has_value());
    }
    // max_mag beyond the symbol limit.
    {
        ByteReader reader(make_bytes(2048, 1024, 128, 17));
        EXPECT_FALSE(read_parametric_m3_params(reader, /*limit=*/16, "unit").has_value());
    }
}

TEST(ParametricTableTest, ReadParamEntryRejectsTruncatedPayload) {
    const ParametricM3 params{.z_q = 2458, .p1_q = 1024, .r_q = 108, .max_mag = 5};
    ByteWriter writer;
    write_parametric_m3_entry(writer, params);
    const auto bytes = writer.finish();

    // Truncate the 7 param bytes (tag byte already consumed by the caller).
    for (const size_t size : {0u, 1u, 3u, 6u}) {
        ByteReader reader(std::span<const uint8_t>(bytes.data() + 1, size));
        EXPECT_FALSE(read_parametric_m3_params(reader, /*limit=*/16, "unit").has_value())
            << "size " << size << " must be rejected";
    }
}

TEST(ParametricTableTest, UnknownEncodingTagIsRejected) {
    // The old decoder path: a tag it does not know must fail cleanly
    // instead of misreading the stream. Tag 8 stands in for tag 7 as
    // seen by a decoder built before this slice.
    ByteWriter writer;
    writer.write_u8(8);
    writer.write_u16(0);
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    EXPECT_FALSE(read_coefficient_table(reader, /*num_symbols=*/32, "unit",
                                        /*limit=*/16, /*split=*/false)
                     .has_value());
}
