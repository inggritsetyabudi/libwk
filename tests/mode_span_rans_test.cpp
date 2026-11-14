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

#include "../src/common.h"
#include "../src/mode_span_rans.h"
#include "../src/mode_stream.h"

using namespace wk;

TEST(ModeSpanRansTest, ModesRoundTrip) {
    const std::vector<PredMode> modes = {
        PredMode::DC, PredMode::DC, PredMode::TM, PredMode::DC,
        PredMode::D45, PredMode::H, PredMode::DC, PredMode::V,
        PredMode::DC, PredMode::DC, PredMode::TM, PredMode::D117,
    };
    ByteWriter writer;
    auto written = write_rans_prediction_modes(writer, modes);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto decoded = read_rans_prediction_modes(reader, modes.size(), "test");
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, modes);
    EXPECT_TRUE(reader.at_end());
}

TEST(ModeSpanRansTest, SpansRoundTrip) {
    // Skewed span distribution (many small spans), like a real photo tile.
    std::vector<uint8_t> spans;
    for (int i = 0; i < 200; ++i) {
        spans.push_back(static_cast<uint8_t>(i % 7));
    }
    spans.push_back(120);
    spans.push_back(200);
    ByteWriter writer;
    auto written = write_rans_coefficient_spans(writer, spans);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto decoded = read_rans_coefficient_spans(reader, spans.size(), "test");
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, spans);
    EXPECT_TRUE(reader.at_end());
}

TEST(ModeSpanRansTest, ModesBeatPackingOnSkewedData) {
    // A tile where one mode dominates: rANS must beat the 4-bit packing.
    std::vector<PredMode> modes(1000, PredMode::DC);
    for (size_t i = 0; i < modes.size(); i += 10) {
        modes[i] = PredMode::H;
    }
    ByteWriter rans_writer;
    auto written = write_rans_prediction_modes(rans_writer, modes);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto rans_bytes = rans_writer.finish().size();

    auto packed = pack_prediction_modes(modes);
    ASSERT_TRUE(packed.has_value()) << packed.error().message;
    EXPECT_LT(rans_bytes, packed->size());
}

TEST(ModeSpanRansTest, SpansBeatRawOnSkewedData) {
    // Spans mostly small: rANS must beat 1 byte per span.
    std::vector<uint8_t> spans(1000, 3);
    for (size_t i = 0; i < spans.size(); i += 5) {
        spans[i] = 12;
    }
    ByteWriter rans_writer;
    auto written = write_rans_coefficient_spans(rans_writer, spans);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    EXPECT_LT(rans_writer.finish().size(), spans.size());
}

TEST(ModeSpanRansTest, LargeTileModesRoundTrip) {
    // A realistic tile: 1024 blocks (32x32), skewed mode distribution
    // like a smooth ramp (DC and H dominate, planar appears).
    std::vector<PredMode> modes;
    modes.reserve(1024);
    for (int i = 0; i < 1024; ++i) {
        if (i % 16 == 0) {
            modes.push_back(PredMode::PLANAR);
        } else if (i % 5 == 0) {
            modes.push_back(PredMode::H);
        } else if (i % 7 == 0) {
            modes.push_back(PredMode::V);
        } else {
            modes.push_back(PredMode::DC);
        }
    }
    ByteWriter writer;
    auto written = write_rans_prediction_modes(writer, modes);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto decoded = read_rans_prediction_modes(reader, modes.size(), "test");
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, modes);
    EXPECT_TRUE(reader.at_end());
}

TEST(ModeSpanRansTest, LargeTileSpansRoundTrip) {
    // A realistic tile: 1024 spans, skewed toward small values with
    // occasional large ones, like a photo tile.
    std::vector<uint8_t> spans;
    spans.reserve(1024);
    for (int i = 0; i < 1024; ++i) {
        if (i % 32 == 0) {
            spans.push_back(45);
        } else if (i % 11 == 0) {
            spans.push_back(20);
        } else {
            spans.push_back(static_cast<uint8_t>(2 + (i % 5)));
        }
    }
    ByteWriter writer;
    auto written = write_rans_coefficient_spans(writer, spans);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    const auto bytes = writer.finish();

    ByteReader reader(bytes);
    auto decoded = read_rans_coefficient_spans(reader, spans.size(), "test");
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, spans);
    EXPECT_TRUE(reader.at_end());
}

TEST(ModeSpanRansTest, RejectsEmptyModes) {
    ByteWriter writer;
    auto written = write_rans_prediction_modes(writer, {});
    EXPECT_FALSE(written.has_value());
}

TEST(ModeSpanRansTest, RejectsEmptySpans) {
    ByteWriter writer;
    auto written = write_rans_coefficient_spans(writer, {});
    EXPECT_FALSE(written.has_value());
}

TEST(ModeSpanRansTest, TruncatedStreamFailsDecode) {
    const std::vector<PredMode> modes = {PredMode::DC, PredMode::H, PredMode::V, PredMode::TM,
                                         PredMode::DC, PredMode::DC, PredMode::H, PredMode::D45};
    ByteWriter writer;
    auto written = write_rans_prediction_modes(writer, modes);
    ASSERT_TRUE(written.has_value()) << written.error().message;
    auto bytes = writer.finish();
    // Drop the last byte: the size-prefixed rANS payload is short.
    bytes.pop_back();
    ByteReader reader(bytes);
    auto decoded = read_rans_prediction_modes(reader, modes.size(), "test");
    EXPECT_FALSE(decoded.has_value());
}
