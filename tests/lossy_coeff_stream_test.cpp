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
#include <utility>

#include "../src/core/coeff_table_bank_stream.h"
#include "../src/core/lossy_coeff_stream.h"
#include "../src/core/parametric_table.h"

using namespace wk;

namespace {

[[nodiscard]] DctBlockI16 make_block(std::initializer_list<std::pair<int, int>> coeffs) {
    DctBlockI16 block{};
    for (const auto& [index, value] : coeffs) {
        block[static_cast<size_t>(index)] = static_cast<int16_t>(value);
    }
    return block;
}

void expect_blocks_equal(std::span<const DctBlockI16> actual,
                         std::span<const DctBlockI16> expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t block_index = 0; block_index < expected.size(); ++block_index) {
        EXPECT_EQ(actual[block_index], expected[block_index]) << "block " << block_index;
    }
}

}

TEST(LossyCoeffStreamTest, PlanePayloadRoundTripsAcrossTableModes) {
    const std::array<DctBlockI16, 3> blocks = {
        make_block({{0, 12}, {1, -4}, {3, 2}}),
        make_block({{0, -12}, {1, 4}, {2, 1}}),
        make_block({{0, 6}})
    };
    const std::array<uint8_t, 3> spans = {4, 3, 1};

    for (const LossyCoeffStreamConfig config : {
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = false,
                                    .split_magnitude_signs = false},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = false},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = false,
                                    .use_table_bank = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = false,
                                    .use_table_bank = true,
                                    .use_table_cluster_selection = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_significance_maps = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_table_bank = true,
                                    .use_significance_maps = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_table_bank = true,
                                    .elide_single_symbol_streams = true,
                                    .use_significance_maps = true,
                                    .use_adaptive_sign_streams = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_table_bank = true},
         }) {
        auto payload = encode_lossy_plane_payload(blocks, spans, 4, config);
        ASSERT_TRUE(payload.has_value()) << payload.error().message;

        ByteReader reader(*payload);
        auto decoded = decode_lossy_plane_payload(reader, spans, 4, config);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        expect_blocks_equal(*decoded, blocks);
        EXPECT_EQ(reader.remaining(), 0u);
    }
}

TEST(LossyCoeffStreamTest, SharedChromaPayloadRoundTripsAcrossTableModes) {
    const std::array<DctBlockI16, 3> cb_blocks = {
        make_block({{0, 3}, {1, -2}, {2, 1}}),
        make_block({{0, -3}, {1, 2}, {3, -1}}),
        make_block({{0, 1}})
    };
    const std::array<DctBlockI16, 3> cr_blocks = {
        make_block({{0, -5}, {1, 2}, {2, -1}}),
        make_block({{0, 5}, {1, -2}, {3, 1}}),
        make_block({{0, -1}})
    };
    const std::array<uint8_t, 3> spans = {4, 4, 1};

    for (const LossyCoeffStreamConfig config : {
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = false,
                                    .split_magnitude_signs = false},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = false},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = false,
                                    .use_table_bank = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = false,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = false,
                                    .use_table_bank = true,
                                    .use_table_cluster_selection = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_significance_maps = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_table_bank = true,
                                    .use_significance_maps = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_table_bank = true,
                                    .elide_single_symbol_streams = true,
                                    .use_significance_maps = true,
                                    .use_adaptive_sign_streams = true},
             LossyCoeffStreamConfig{.use_plane_max_coeff_span = true,
                                    .adaptive_coefficient_tables = true,
                                    .split_magnitude_signs = true,
                                    .use_table_bank = true},
         }) {
        auto payload = encode_lossy_chroma_payload(cb_blocks, cr_blocks, spans, 4, config);
        ASSERT_TRUE(payload.has_value()) << payload.error().message;

        ByteReader reader(*payload);
        auto decoded = decode_lossy_chroma_payload(reader, spans, 4, config);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        expect_blocks_equal(decoded->cb_blocks, cb_blocks);
        expect_blocks_equal(decoded->cr_blocks, cr_blocks);
        EXPECT_EQ(reader.remaining(), 0u);
    }
}

TEST(LossyCoeffStreamTest, SharedChromaPayloadShrinksWhenPlanesShareStatistics) {
    const std::array<DctBlockI16, 8> cb_blocks = {
        make_block({{0, 4}, {1, 1}, {2, -1}}),
        make_block({{0, 4}, {1, 0}, {2, 1}}),
        make_block({{0, -4}, {1, -1}, {2, 0}}),
        make_block({{0, -4}, {1, 1}, {2, 0}}),
        make_block({{0, 2}, {1, 0}, {2, 1}}),
        make_block({{0, 2}, {1, -1}, {2, -1}}),
        make_block({{0, -2}, {1, 1}, {2, 1}}),
        make_block({{0, -2}, {1, 0}, {2, -1}}),
    };
    const std::array<DctBlockI16, 8> cr_blocks = {
        make_block({{0, -4}, {1, -1}, {2, 0}}),
        make_block({{0, 2}, {1, 0}, {2, 1}}),
        make_block({{0, -2}, {1, 1}, {2, 1}}),
        make_block({{0, 4}, {1, 1}, {2, -1}}),
        make_block({{0, -4}, {1, 1}, {2, 0}}),
        make_block({{0, 2}, {1, -1}, {2, -1}}),
        make_block({{0, -2}, {1, 0}, {2, -1}}),
        make_block({{0, 4}, {1, 0}, {2, 1}}),
    };
    const std::array<uint8_t, 8> spans = {3, 3, 3, 3, 3, 3, 3, 3};
    const LossyCoeffStreamConfig config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
    };

    auto independent_cb = encode_lossy_plane_payload(cb_blocks, spans, 3, config);
    ASSERT_TRUE(independent_cb.has_value()) << independent_cb.error().message;
    auto independent_cr = encode_lossy_plane_payload(cr_blocks, spans, 3, config);
    ASSERT_TRUE(independent_cr.has_value()) << independent_cr.error().message;
    auto shared = encode_lossy_chroma_payload(cb_blocks, cr_blocks, spans, 3, config);
    ASSERT_TRUE(shared.has_value()) << shared.error().message;

    EXPECT_LT(shared->size(), independent_cb->size() + independent_cr->size());
}

TEST(LossyCoeffStreamTest, TableClusterSelectionShrinksBankedPlanePayload) {
    std::array<DctBlockI16, 16> blocks{};
    const std::array<int16_t, 16> coeff1 = {
        2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, -1, -1, -1, -1
    };
    const std::array<int16_t, 16> coeff2 = {
        2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, -1, -1, -1, -1, -1
    };
    for (size_t index = 0; index < blocks.size(); ++index) {
        blocks[index][0] = static_cast<int16_t>((index % 5) - 2);
        blocks[index][1] = coeff1[index];
        blocks[index][2] = coeff2[index];
    }
    const std::array<uint8_t, 16> spans = {
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
    };

    const LossyCoeffStreamConfig base_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
    };
    const LossyCoeffStreamConfig cluster_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
        .use_table_cluster_selection = true,
    };

    auto base_payload = encode_lossy_plane_payload(blocks, spans, 3, base_config);
    ASSERT_TRUE(base_payload.has_value()) << base_payload.error().message;
    auto cluster_payload = encode_lossy_plane_payload(blocks, spans, 3, cluster_config);
    ASSERT_TRUE(cluster_payload.has_value()) << cluster_payload.error().message;

    EXPECT_LT(cluster_payload->size(), base_payload->size());

    ByteReader reader(*cluster_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 3, cluster_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);
}

TEST(LossyCoeffStreamTest, PooledBandTableShrinksBankedPlanePayload) {
    // Positions 32..63 share one log-frequency band, but each position
    // uses a distinct symbol (besides the common 0/1): no other
    // position's table can support it, so plain cluster reuse is
    // rejected everywhere, while one pooled band table supports all
    // of them and collapses 32 bank entries into one.
    std::array<DctBlockI16, 32> blocks{};
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        for (int pos = 32; pos < 64; ++pos) {
            int16_t value = 0;
            if (block_index % 16 == 0) {
                value = 1;
            } else if (block_index % 16 == 1) {
                value = static_cast<int16_t>(pos - 30);
            }
            blocks[block_index][static_cast<size_t>(pos)] = value;
        }
    }
    const std::array<uint8_t, 32> spans = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
    };

    const LossyCoeffStreamConfig base_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
    };
    const LossyCoeffStreamConfig cluster_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
        .use_table_cluster_selection = true,
    };

    auto base_payload = encode_lossy_plane_payload(blocks, spans, 64, base_config);
    ASSERT_TRUE(base_payload.has_value()) << base_payload.error().message;
    auto cluster_payload = encode_lossy_plane_payload(blocks, spans, 64, cluster_config);
    ASSERT_TRUE(cluster_payload.has_value()) << cluster_payload.error().message;

    EXPECT_LT(cluster_payload->size(), base_payload->size());

    ByteReader reader(*cluster_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 64, cluster_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);
}

TEST(LossyCoeffStreamTest, SingleSymbolStreamElisionShrinksSplitMagnitudePayload) {
    const std::array<DctBlockI16, 5> blocks = {
        make_block({{0, 3}, {1, -2}}),
        make_block({{0, -3}, {1, 2}}),
        make_block({{0, 3}, {1, -2}}),
        make_block({{0, -3}, {1, 2}}),
        make_block({{0, 3}, {1, -2}})
    };
    const std::array<uint8_t, 5> spans = {2, 2, 2, 2, 2};

    const LossyCoeffStreamConfig base_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = true,
    };
    const LossyCoeffStreamConfig elided_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = true,
        .elide_single_symbol_streams = true,
    };

    auto base_payload = encode_lossy_plane_payload(blocks, spans, 2, base_config);
    ASSERT_TRUE(base_payload.has_value()) << base_payload.error().message;
    auto elided_payload = encode_lossy_plane_payload(blocks, spans, 2, elided_config);
    ASSERT_TRUE(elided_payload.has_value()) << elided_payload.error().message;

    EXPECT_LT(elided_payload->size(), base_payload->size());

    ByteReader reader(*elided_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 2, elided_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);
}

TEST(LossyCoeffStreamTest, SignificanceMapsShrinkSparseSplitPayload) {
    const std::array<DctBlockI16, 8> blocks = {
        make_block({{0, 12}, {1, 2}, {2, 1}}),
        make_block({{0, 11}}),
        make_block({{0, -12}, {2, -1}}),
        make_block({{0, 10}}),
        make_block({{0, -11}, {1, -2}}),
        make_block({{0, 9}}),
        make_block({{0, 8}, {2, 1}}),
        make_block({{0, -8}})
    };
    const std::array<uint8_t, 8> spans = {3, 3, 3, 3, 3, 3, 3, 3};

    const LossyCoeffStreamConfig base_config{
        .use_plane_max_coeff_span = true,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = true,
        .use_table_bank = true,
        .elide_single_symbol_streams = true,
    };
    const LossyCoeffStreamConfig significance_config{
        .use_plane_max_coeff_span = true,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = true,
        .use_table_bank = true,
        .elide_single_symbol_streams = true,
        .use_significance_maps = true,
    };

    auto base_payload = encode_lossy_plane_payload(blocks, spans, 3, base_config);
    ASSERT_TRUE(base_payload.has_value()) << base_payload.error().message;
    auto significance_payload = encode_lossy_plane_payload(blocks, spans, 3, significance_config);
    ASSERT_TRUE(significance_payload.has_value()) << significance_payload.error().message;

    EXPECT_LT(significance_payload->size(), base_payload->size());

    ByteReader reader(*significance_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 3, significance_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);
}

TEST(LossyCoeffStreamTest, AdaptiveSignModesShrinkUniformSignPayload) {
    const std::array<DctBlockI16, 12> blocks = {
        make_block({{0, 12}, {1, 3}, {2, 1}}),
        make_block({{0, 11}, {1, 2}, {2, 1}}),
        make_block({{0, 10}, {1, 3}}),
        make_block({{0, 9}, {1, 2}}),
        make_block({{0, 8}, {1, 3}}),
        make_block({{0, 7}, {1, 2}}),
        make_block({{0, 6}, {1, 3}}),
        make_block({{0, 5}, {1, 2}}),
        make_block({{0, 4}, {1, 3}}),
        make_block({{0, 3}, {1, 2}}),
        make_block({{0, 2}, {1, 3}}),
        make_block({{0, 1}, {1, 2}})
    };
    const std::array<uint8_t, 12> spans = {3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

    const LossyCoeffStreamConfig base_config{
        .use_plane_max_coeff_span = true,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = true,
        .use_table_bank = true,
        .elide_single_symbol_streams = true,
    };
    const LossyCoeffStreamConfig adaptive_sign_config{
        .use_plane_max_coeff_span = true,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = true,
        .use_table_bank = true,
        .elide_single_symbol_streams = true,
        .use_adaptive_sign_streams = true,
    };

    auto base_payload = encode_lossy_plane_payload(blocks, spans, 3, base_config);
    ASSERT_TRUE(base_payload.has_value()) << base_payload.error().message;
    auto adaptive_payload = encode_lossy_plane_payload(blocks, spans, 3, adaptive_sign_config);
    ASSERT_TRUE(adaptive_payload.has_value()) << adaptive_payload.error().message;

    EXPECT_LT(adaptive_payload->size(), base_payload->size());

    ByteReader reader(*adaptive_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 3, adaptive_sign_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);
}

TEST(LossyCoeffStreamTest, ParametricM3ShrinksBankedPlanePayload) {
    // 64 blocks x 16 positions. Each position has a distinct wide table
    // (zeros ~72%, ones ~15%, a geometric-like tail out to magnitude
    // 6..10 that differs per position), so plain clustering cannot
    // collapse them, while one 8-byte parametric entry replaces each
    // ~47-byte dense-range table.
    std::array<DctBlockI16, 64> blocks{};
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        for (int pos = 0; pos < 16; ++pos) {
            const int v = static_cast<int>(
                (block_index * 5 + static_cast<size_t>(pos) * 11) % 97);
            int16_t magnitude = 0;
            if (v >= 70 && v < 85) {
                magnitude = 1;
            } else if (v >= 85 && v < 93) {
                magnitude = 2;
            } else if (v >= 93 && v < 96) {
                magnitude = 3;
            } else if (v >= 96) {
                magnitude = static_cast<int16_t>(6 + (pos % 5));
            }
            blocks[block_index][static_cast<size_t>(pos)] =
                ((block_index + static_cast<size_t>(pos)) % 2 == 0)
                    ? magnitude
                    : static_cast<int16_t>(-magnitude);
        }
    }
    const std::array<uint8_t, 64> spans = {
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    };

    const LossyCoeffStreamConfig base_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
    };
    const LossyCoeffStreamConfig parametric_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
        .use_parametric_m3_tables = true,
    };

    auto base_payload = encode_lossy_plane_payload(blocks, spans, 16, base_config);
    ASSERT_TRUE(base_payload.has_value()) << base_payload.error().message;
    auto parametric_payload =
        encode_lossy_plane_payload(blocks, spans, 16, parametric_config);
    ASSERT_TRUE(parametric_payload.has_value()) << parametric_payload.error().message;

    EXPECT_LT(parametric_payload->size(), base_payload->size());

    ByteReader reader(*parametric_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 16, parametric_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);
}

TEST(LossyCoeffStreamTest, ParametricM3KeepsBadFitsAsRegularTables) {
    // Position 0 has a distribution a geometric tail cannot fit: two
    // spikes of equal mass at magnitudes 1 and 40 and nothing between.
    // The trial must leave it a regular table (winning-only), while the
    // well-behaved position 1 still converts. The bank is the first
    // thing in the payload for this config (no significance maps, no
    // adaptive sign streams), so the test reads it back directly.
    std::array<DctBlockI16, 64> blocks{};
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        for (int pos = 0; pos < 16; ++pos) {
            int16_t magnitude = 0;
            if (pos == 0) {
                magnitude = (block_index % 2 == 0) ? 1 : 40;
            } else {
                const int v = static_cast<int>(
                    (block_index * 5 + static_cast<size_t>(pos) * 11) % 97);
                if (v >= 70 && v < 85) {
                    magnitude = 1;
                } else if (v >= 85 && v < 93) {
                    magnitude = 2;
                } else if (v >= 93 && v < 96) {
                    magnitude = 3;
                } else if (v >= 96) {
                    magnitude = static_cast<int16_t>(6 + (pos % 5));
                }
            }
            blocks[block_index][static_cast<size_t>(pos)] =
                ((block_index + static_cast<size_t>(pos)) % 2 == 0)
                    ? magnitude
                    : static_cast<int16_t>(-magnitude);
        }
    }
    const std::array<uint8_t, 64> spans = {
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    };

    const LossyCoeffStreamConfig parametric_config{
        .use_plane_max_coeff_span = false,
        .adaptive_coefficient_tables = true,
        .split_magnitude_signs = false,
        .use_table_bank = true,
        .use_parametric_m3_tables = true,
    };

    auto parametric_payload =
        encode_lossy_plane_payload(blocks, spans, 16, parametric_config);
    ASSERT_TRUE(parametric_payload.has_value()) << parametric_payload.error().message;

    ByteReader reader(*parametric_payload);
    auto decoded = decode_lossy_plane_payload(reader, spans, 16, parametric_config);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    expect_blocks_equal(*decoded, blocks);

    // The bank leads the payload: position 0 must still hold its
    // regular two-symbol table, position 1 the fitted parametric one.
    ByteReader bank_reader(*parametric_payload);
    auto bank_tables = read_coefficient_table_bank(bank_reader, 16, 2049, "lossy",
                                                    /*limit=*/1024, /*split=*/false);
    ASSERT_TRUE(bank_tables.has_value()) << bank_tables.error().message;

    std::vector<uint32_t> pos0_counts(2049, 0);
    pos0_counts[1025] = 32;  // +1 on even blocks
    pos0_counts[984] = 32;   // -40 on odd blocks
    LossyCoeffTable pos0_regular;
    pos0_regular.build_from_counts(pos0_counts.data(), 2049);
    for (int symbol = 0; symbol < 2049; ++symbol) {
        EXPECT_EQ((*bank_tables)[0].symbol(symbol).freq,
                  pos0_regular.symbol(symbol).freq)
            << "position 0 must stay a regular table, symbol " << symbol;
    }

    std::vector<uint32_t> pos1_counts(2049, 0);
    for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        const int v = static_cast<int>((block_index * 5 + 11) % 97);
        int16_t magnitude = 0;
        if (v >= 70 && v < 85) {
            magnitude = 1;
        } else if (v >= 85 && v < 93) {
            magnitude = 2;
        } else if (v >= 93 && v < 96) {
            magnitude = 3;
        } else if (v >= 96) {
            magnitude = 7;
        }
        const int16_t value = ((block_index + 1) % 2 == 0)
                                  ? magnitude
                                  : static_cast<int16_t>(-magnitude);
        pos1_counts[static_cast<size_t>(value + 1024)]++;
    }
    auto pos1_params = fit_parametric_m3(pos1_counts, /*limit=*/1024, /*split=*/false);
    ASSERT_TRUE(pos1_params.has_value());
    const auto pos1_fitted =
        reconstruct_parametric_m3(*pos1_params, /*limit=*/1024, /*split=*/false);
    for (int symbol = 0; symbol < 2049; ++symbol) {
        EXPECT_EQ((*bank_tables)[1].symbol(symbol).freq,
                  pos1_fitted.symbol(symbol).freq)
            << "position 1 must convert to a parametric entry, symbol " << symbol;
    }
}
