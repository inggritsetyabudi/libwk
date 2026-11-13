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
#include <algorithm>
#include <array>
#include <optional>
#include <wk/wk.hpp>
#include <wk/wk.h>
#include "../src/common.h"
#include "../src/coeff_presence_stream.h"
#include "../src/coeff_sign_stream.h"
#include "../src/coeff_table_bank_stream.h"
#include "../src/container.h"
#include "../src/coeff_span_stream.h"
#include "../src/coeff_table_stream.h"
#include "../src/image_io.h"
#include "../src/metrics.h"
#include "../src/mode_stream.h"
#include "../src/rans.h"
#include <cmath>
#include <filesystem>

using namespace wk;

namespace {

Image make_rgb_gradient(uint32_t width, uint32_t height) {
    Image image(width, height, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = static_cast<uint8_t>((x * 255u) / std::max(width - 1, 1u));
            row[x * 3 + 1] = static_cast<uint8_t>((y * 255u) / std::max(height - 1, 1u));
            row[x * 3 + 2] = static_cast<uint8_t>(((x + y) * 255u) / std::max(width + height - 2, 1u));
        }
    }
    return image;
}

Image make_dark_color_scene(uint32_t width, uint32_t height) {
    Image image(width, height, BitDepth::Bits8, false);
    auto clamp_byte = [](float value) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
    };
    auto glow = [](float dx, float dy, float sigma) -> float {
        return std::exp(-(dx * dx + dy * dy) / sigma);
    };

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        const float fy = static_cast<float>(y) / std::max(height - 1, 1u);
        for (uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / std::max(width - 1, 1u);
            const float amber = glow(fx - 0.32f, fy - 0.24f, 0.010f);
            const float cyan = glow(fx - 0.72f, fy - 0.36f, 0.014f);
            const float magenta = glow(fx - 0.50f, fy - 0.68f, 0.020f);
            const float horizon = std::exp(-std::abs(fy - 0.55f) * 18.0f);
            const float texture = 0.5f + 0.5f * std::sin(fx * 28.0f + fy * 8.0f);
            const float reflection = fy > 0.55f ? (fy - 0.55f) * 1.8f : 0.0f;
            const float neon_band = (fy > 0.48f && fy < 0.52f) ? 1.0f - std::min(1.0f, std::abs(fx - 0.52f) * 5.0f) : 0.0f;

            float r = 0.03f + 0.05f * (1.0f - fy) + 0.72f * amber + 0.24f * magenta + 0.10f * horizon + 0.03f * texture;
            float g = 0.04f + 0.06f * (1.0f - fy) + 0.40f * amber + 0.46f * cyan + 0.08f * horizon + 0.02f * texture;
            float b = 0.08f + 0.14f * (1.0f - fy) + 0.82f * cyan + 0.18f * magenta + 0.10f * reflection + 0.05f * texture;

            if (fy > 0.55f) {
                r += 0.25f * amber * reflection;
                g += 0.15f * cyan * reflection;
                b += 0.32f * (cyan + magenta) * reflection;
            }
            r += 0.18f * neon_band;
            g += 0.12f * neon_band;
            b += 0.28f * neon_band;

            row[x * 3 + 0] = clamp_byte(std::clamp(r, 0.0f, 1.0f));
            row[x * 3 + 1] = clamp_byte(std::clamp(g, 0.0f, 1.0f));
            row[x * 3 + 2] = clamp_byte(std::clamp(b, 0.0f, 1.0f));
        }
    }
    return image;
}

std::filesystem::path photos_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "photos";
}

std::filesystem::path find_photo_named(std::string_view name) {
    const std::filesystem::path root = photos_root();
    const std::array<std::filesystem::path, 3> preferred = {
        root / std::string(name),
        root / "people" / std::string(name),
        root / "scenery" / std::string(name)
    };
    for (const auto& candidate : preferred) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }
        if (it->path().filename() == name) {
            return it->path();
        }
    }
    return {};
}

std::filesystem::path sample_photo_path() {
    return find_photo_named("ember-7f3a.jpg");
}

constexpr size_t kTileHeaderBytes = 9u;
constexpr size_t kQuantTableBytes = 64u * sizeof(uint16_t) * 2u;
constexpr size_t kBlockDimensionBytes = 4u * sizeof(uint16_t);
constexpr size_t kLayoutTagBytes = sizeof(uint32_t);
constexpr size_t kLossySyntaxBaseOffset = kTileHeaderBytes + kQuantTableBytes + kBlockDimensionBytes;

struct LossyTileSyntaxInfo {
    uint32_t layout_tag = 0;
    uint8_t syntax_flags = 0;
    size_t stream_offset = 0;
    bool adaptive_spans = false;
    bool plane_extents = false;
    bool split_magnitude_signs = false;
    bool shared_chroma_tables = false;
    bool coefficient_table_bank = false;
    bool elide_single_symbol_streams = false;
    bool coefficient_significance_maps = false;
    bool adaptive_coefficient_signs = false;
};

Result<LossyTileSyntaxInfo> parse_lossy_tile_syntax(std::span<const uint8_t> payload) {
    if (payload.size() < kLossySyntaxBaseOffset + kLayoutTagBytes) {
        return std::unexpected(Error{ErrorCode::TruncatedInput, "lossy payload is missing layout tag"});
    }

    LossyTileSyntaxInfo info;
    info.layout_tag = read_le32(payload.data() + kLossySyntaxBaseOffset);
    info.stream_offset = kLossySyntaxBaseOffset + kLayoutTagBytes;
    if (info.layout_tag == kLossyTileLayoutTagV6) {
        if (payload.size() <= info.stream_offset) {
            return std::unexpected(Error{ErrorCode::TruncatedInput, "lossy payload is missing syntax flags"});
        }
        info.syntax_flags = payload[info.stream_offset++];
        info.adaptive_spans =
            (info.syntax_flags & kLossyTileSyntaxFlagAdaptiveSpanStreams) != 0;
        info.plane_extents =
            (info.syntax_flags & kLossyTileSyntaxFlagPlaneCoeffExtents) != 0;
        info.split_magnitude_signs =
            (info.syntax_flags & kLossyTileSyntaxFlagSplitMagnitudeSigns) != 0;
        info.shared_chroma_tables =
            (info.syntax_flags & kLossyTileSyntaxFlagSharedChromaCoeffTables) != 0;
        info.coefficient_table_bank =
            (info.syntax_flags & kLossyTileSyntaxFlagCoefficientTableBank) != 0;
        info.elide_single_symbol_streams =
            (info.syntax_flags & kLossyTileSyntaxFlagElideSingleSymbolStreams) != 0;
        info.coefficient_significance_maps =
            (info.syntax_flags & kLossyTileSyntaxFlagCoefficientSignificanceMaps) != 0;
        info.adaptive_coefficient_signs =
            (info.syntax_flags & kLossyTileSyntaxFlagAdaptiveCoefficientSigns) != 0;
    } else if (info.layout_tag == kLossyTileLayoutTagV5) {
        info.plane_extents = true;
    }

    return info;
}

size_t encoded_coefficient_span_stream_bytes(std::span<const uint8_t> payload,
                                             size_t offset,
                                             bool adaptive_spans,
                                             size_t expected_count) {
    if (!adaptive_spans) {
        return sizeof(uint16_t) + packed_coefficient_span_bytes(expected_count);
    }

    const uint16_t header = read_le16(payload.data() + offset);
    const uint16_t encoding = static_cast<uint16_t>(header & 0xC000u);
    const uint16_t encoded_bytes = static_cast<uint16_t>(header & 0x3FFFu);
    switch (encoding) {
    case 0x0000u:
    case 0x8000u:
        return sizeof(uint16_t) + encoded_bytes;
    case 0x4000u:
    default:
        return sizeof(uint16_t);
    }
}

[[nodiscard]] std::optional<int> single_symbol_table_value(const LossyCoeffTable& table,
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

}

TEST(RoundtripTest, LosslessSmallImage) {
    Image image(16, 16, BitDepth::Bits8, true);
    for (uint32_t y = 0; y < 16; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 16; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>(x * 16);
            row[x * 4 + 1] = static_cast<uint8_t>(y * 16);
            row[x * 4 + 2] = static_cast<uint8_t>((x + y) * 8);
            row[x * 4 + 3] = 255;
        }
    }

    EncoderConfig config;
    config.lossless = true;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width(), 16u);
    EXPECT_EQ(decoded->height(), 16u);
    EXPECT_EQ(image.pixels().size(), decoded->pixels().size());
    EXPECT_TRUE(std::equal(image.pixels().begin(), image.pixels().end(), decoded->pixels().begin()));
}

TEST(RoundtripTest, LossySolidColor) {
    Image image(64, 64, BitDepth::Bits8, true);
    for (uint32_t y = 0; y < 64; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 64; ++x) {
            row[x * 4 + 0] = 128;
            row[x * 4 + 1] = 64;
            row[x * 4 + 2] = 192;
            row[x * 4 + 3] = 255;
        }
    }

    EncoderConfig config;
    config.quality = 90.0f;
    config.tile_size_log2 = 6;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width(), 64u);
    EXPECT_EQ(decoded->height(), 64u);

    auto metrics = wk::metrics::compare_images(image, *decoded);
    ASSERT_TRUE(metrics.has_value()) << "metrics compare failed: " << metrics.error().message;
    EXPECT_GT(metrics->psnr, 25.0) << "PSNR too low: " << metrics->psnr;
    EXPECT_GT(metrics->ssim, 0.95) << "SSIM too low: " << metrics->ssim;
}

TEST(RoundtripTest, HigherLossyQualityTradesSizeForQuality) {
    Image image = make_rgb_gradient(192, 192);

    EncoderConfig config75;
    config75.quality = 75.0f;
    config75.subsampling = Subsampling::YUV444;

    EncoderConfig config85;
    config85.quality = 85.0f;
    config85.subsampling = Subsampling::YUV444;

    auto encoded75 = encode(image, config75);
    ASSERT_TRUE(encoded75.has_value()) << encoded75.error().message;
    auto decoded75 = decode(*encoded75);
    ASSERT_TRUE(decoded75.has_value()) << decoded75.error().message;
    auto metrics75 = wk::metrics::compare_images(image, *decoded75);
    ASSERT_TRUE(metrics75.has_value()) << "metrics compare failed: " << metrics75.error().message;

    auto encoded85 = encode(image, config85);
    ASSERT_TRUE(encoded85.has_value()) << encoded85.error().message;
    auto decoded85 = decode(*encoded85);
    ASSERT_TRUE(decoded85.has_value()) << decoded85.error().message;
    auto metrics85 = wk::metrics::compare_images(image, *decoded85);
    ASSERT_TRUE(metrics85.has_value()) << "metrics compare failed: " << metrics85.error().message;

    EXPECT_GT(encoded85->size(), encoded75->size());
    EXPECT_GT(metrics75->psnr, 46.0);
    EXPECT_GT(metrics75->ssim, 0.98);
    EXPECT_GT(metrics75->artifacts.chroma_psnr, 50.0);
    EXPECT_GE(metrics85->psnr, metrics75->psnr - 0.6) << "Higher quality should not materially regress PSNR";
    EXPECT_GE(metrics85->ssim, metrics75->ssim - 0.003) << "Higher quality should not materially regress SSIM";
    EXPECT_GE(metrics85->artifacts.chroma_psnr, metrics75->artifacts.chroma_psnr - 0.8)
        << "Higher quality should not materially regress chroma PSNR";
}

TEST(RoundtripTest, DefaultLossyTilePolicyUses1024ButLosslessStays512) {
    Image image = make_rgb_gradient(640, 384);

    EncoderConfig lossy_auto;
    lossy_auto.quality = 85.0f;
    lossy_auto.subsampling = Subsampling::YUV444;

    auto encoded_lossy = encode(image, lossy_auto);
    ASSERT_TRUE(encoded_lossy.has_value()) << encoded_lossy.error().message;

    auto lossy_info = get_info(*encoded_lossy);
    ASSERT_TRUE(lossy_info.has_value()) << lossy_info.error().message;
    EXPECT_EQ(lossy_info->tile_size, 1024u);

    auto lossy_file = parse_container(*encoded_lossy);
    ASSERT_TRUE(lossy_file.has_value()) << lossy_file.error().message;
    EXPECT_EQ(lossy_file->header.tile_size_log2, 10u);

    EncoderConfig lossy_512 = lossy_auto;
    lossy_512.tile_size_log2 = 9;

    auto encoded_lossy_512 = encode(image, lossy_512);
    ASSERT_TRUE(encoded_lossy_512.has_value()) << encoded_lossy_512.error().message;
    EXPECT_LT(encoded_lossy->size(), encoded_lossy_512->size());

    EncoderConfig lossless;
    lossless.lossless = true;

    auto encoded_lossless = encode(image, lossless);
    ASSERT_TRUE(encoded_lossless.has_value()) << encoded_lossless.error().message;

    auto lossless_info = get_info(*encoded_lossless);
    ASSERT_TRUE(lossless_info.has_value()) << lossless_info.error().message;
    EXPECT_EQ(lossless_info->tile_size, 512u);

    auto lossless_file = parse_container(*encoded_lossless);
    ASSERT_TRUE(lossless_file.has_value()) << lossless_file.error().message;
    EXPECT_EQ(lossless_file->header.tile_size_log2, 9u);
}

TEST(RoundtripTest, HigherLossyQualityImprovesDarkChromaStability) {
    Image image = make_dark_color_scene(192, 192);

    EncoderConfig config75;
    config75.quality = 75.0f;
    config75.subsampling = Subsampling::YUV444;

    EncoderConfig config85;
    config85.quality = 85.0f;
    config85.subsampling = Subsampling::YUV444;

    auto encoded75 = encode(image, config75);
    ASSERT_TRUE(encoded75.has_value()) << encoded75.error().message;
    auto decoded75 = decode(*encoded75);
    ASSERT_TRUE(decoded75.has_value()) << decoded75.error().message;
    auto metrics75 = wk::metrics::compare_images(image, *decoded75);
    ASSERT_TRUE(metrics75.has_value()) << "metrics compare failed: " << metrics75.error().message;

    auto encoded85 = encode(image, config85);
    ASSERT_TRUE(encoded85.has_value()) << encoded85.error().message;
    auto decoded85 = decode(*encoded85);
    ASSERT_TRUE(decoded85.has_value()) << decoded85.error().message;
    auto metrics85 = wk::metrics::compare_images(image, *decoded85);
    ASSERT_TRUE(metrics85.has_value()) << "metrics compare failed: " << metrics85.error().message;

    EXPECT_GT(encoded85->size(), encoded75->size());
    EXPECT_GT(metrics75->psnr, 24.0);
    EXPECT_GT(metrics75->ssim, 0.90);
    EXPECT_GT(metrics85->artifacts.chroma_psnr, metrics75->artifacts.chroma_psnr + 1.0)
        << "Higher quality should materially improve chroma fidelity in dark color scenes";
    EXPECT_LT(metrics85->artifacts.weighted_chroma_mae, metrics75->artifacts.weighted_chroma_mae)
        << "Higher quality should reduce weighted chroma error in dark color scenes";
}

TEST(RoundtripTest, LossyTransparentAlphaRoundTrip) {
    Image image(48, 48, BitDepth::Bits8, true);
    for (uint32_t y = 0; y < 48; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 48; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>((x * 255u) / 47u);
            row[x * 4 + 1] = static_cast<uint8_t>((y * 255u) / 47u);
            row[x * 4 + 2] = static_cast<uint8_t>(((x + y) * 255u) / 94u);
            row[x * 4 + 3] = static_cast<uint8_t>(((x * y) * 255u) / (47u * 47u));
        }
    }

    EncoderConfig config;
    config.quality = 90.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_TRUE(decoded->has_alpha());
    EXPECT_EQ(decoded->width(), image.width());
    EXPECT_EQ(decoded->height(), image.height());

    auto metrics = wk::metrics::compare_images(image, *decoded);
    ASSERT_TRUE(metrics.has_value()) << "metrics compare failed: " << metrics.error().message;
    EXPECT_GT(metrics->channels[3].psnr, 24.0) << "Alpha PSNR too low: " << metrics->channels[3].psnr;
}

TEST(RoundtripTest, LossyAlphaTileMismatchRejected) {
    Image image(32, 32, BitDepth::Bits8, true);
    for (uint32_t y = 0; y < 32; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 32; ++x) {
            row[x * 4 + 0] = 200;
            row[x * 4 + 1] = 40;
            row[x * 4 + 2] = 120;
            row[x * 4 + 3] = static_cast<uint8_t>((x * 255u) / 31u);
        }
    }

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_FALSE(file->tile_chunks.empty());
    ASSERT_GE(file->tile_chunks.front().payload.size(), 9u);
    file->tile_chunks.front().payload[4] = TILE_HAS_BASE;

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::DecodeFailed);
}

TEST(RoundtripTest, BadEntropyStreamRejected) {
    Image image = make_rgb_gradient(64, 64);

    EncoderConfig config;
    config.lossless = true;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks[0].payload;
    const size_t stream_offset = 9u + 2u + 512u + 4u;
    ASSERT_GT(payload.size(), stream_offset + 3u);
    payload[stream_offset + 0] = 0;
    payload[stream_offset + 1] = 0;
    payload[stream_offset + 2] = 0;
    payload[stream_offset + 3] = 0;

    auto broken_file = write_container(*file);
    ASSERT_TRUE(broken_file.has_value()) << broken_file.error().message;

    auto decoded = decode(*broken_file);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::RansError);
}

TEST(RoundtripTest, PackedModeStreamCorruptionRejected) {
    Image image = make_rgb_gradient(24, 24);

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks.front().payload;
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    const size_t mode_size_offset = syntax->stream_offset;

    ASSERT_GT(payload.size(), mode_size_offset + sizeof(uint16_t) - 1);
    payload[mode_size_offset + 0] = 0;
    payload[mode_size_offset + 1] = 0;

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::PredictionError);
}

TEST(RoundtripTest, PackedCoefficientSpanCorruptionRejected) {
    Image image = make_rgb_gradient(24, 24);

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks.front().payload;
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    const size_t y_blocks = 9;
    const size_t chroma_blocks = 9;
    const size_t y_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(y_blocks);
    const size_t chroma_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(chroma_blocks);
    const size_t span_size_offset = syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes;

    ASSERT_GT(payload.size(), span_size_offset + sizeof(uint16_t) - 1);
    payload[span_size_offset + 0] = 0;
    payload[span_size_offset + 1] = 0xC0;

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::DecodeFailed);
}

TEST(RoundtripTest, CoefficientSignCorruptionRejected) {
    Image image = make_rgb_gradient(24, 24);

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks.front().payload;
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    ASSERT_TRUE(syntax->split_magnitude_signs);

    const size_t y_blocks = 9;
    const size_t chroma_blocks = 9;
    ByteReader reader(std::span<const uint8_t>(payload.data() + syntax->stream_offset,
                                               payload.size() - syntax->stream_offset));

    auto y_modes = read_packed_prediction_modes(reader, y_blocks, "luma");
    ASSERT_TRUE(y_modes.has_value()) << y_modes.error().message;
    auto chroma_modes = read_packed_prediction_modes(reader, chroma_blocks, "chroma");
    ASSERT_TRUE(chroma_modes.has_value()) << chroma_modes.error().message;

    auto y_spans = syntax->adaptive_spans
        ? read_adaptive_coefficient_spans(reader, y_blocks, "luma")
        : read_packed_coefficient_spans(reader, y_blocks, "luma");
    ASSERT_TRUE(y_spans.has_value()) << y_spans.error().message;
    auto chroma_spans = syntax->adaptive_spans
        ? read_adaptive_coefficient_spans(reader, chroma_blocks, "chroma")
        : read_packed_coefficient_spans(reader, chroma_blocks, "chroma");
    ASSERT_TRUE(chroma_spans.has_value()) << chroma_spans.error().message;

    uint8_t y_extent = 64;
    if (syntax->plane_extents) {
        auto y_extent_read = reader.read_u8();
        auto chroma_extent = reader.read_u8();
        ASSERT_TRUE(y_extent_read.has_value()) << y_extent_read.error().message;
        ASSERT_TRUE(chroma_extent.has_value()) << chroma_extent.error().message;
        y_extent = *y_extent_read;
    }

    std::vector<uint8_t> significance_flags(y_extent, 0);
    if (syntax->coefficient_significance_maps) {
        auto flags = unpack_coefficient_presence(
            std::span<const uint8_t>(payload.data() + syntax->stream_offset + reader.position(),
                                     packed_coefficient_presence_bytes(y_extent)),
            y_extent,
            "lossy significance mode");
        ASSERT_TRUE(flags.has_value()) << flags.error().message;
        significance_flags = std::move(*flags);
        auto skipped = reader.read_bytes(packed_coefficient_presence_bytes(y_extent));
        ASSERT_TRUE(skipped.has_value()) << skipped.error().message;
    }
    std::vector<uint8_t> sign_modes(y_extent, kCoefficientSignModeRawPacked);
    if (syntax->adaptive_coefficient_signs) {
        auto flags = unpack_coefficient_sign_modes(
            std::span<const uint8_t>(payload.data() + syntax->stream_offset + reader.position(),
                                     packed_coefficient_sign_mode_bytes(y_extent)),
            y_extent,
            "lossy coefficient sign mode");
        ASSERT_TRUE(flags.has_value()) << flags.error().message;
        sign_modes = std::move(*flags);
        auto skipped = reader.read_bytes(packed_coefficient_sign_mode_bytes(y_extent));
        ASSERT_TRUE(skipped.has_value()) << skipped.error().message;
    }

    std::vector<LossyCoeffTable> y_tables;
    if (syntax->coefficient_table_bank) {
        auto tables = read_coefficient_table_bank(reader, y_extent, 1025, "lossy");
        ASSERT_TRUE(tables.has_value()) << tables.error().message;
        ASSERT_FALSE(tables->empty());
        y_tables = std::move(*tables);
    } else {
        y_tables.reserve(y_extent);
    }
    bool corrupted = false;
    for (size_t coeff_index = 0; coeff_index < y_extent; ++coeff_index) {
        const size_t active_blocks = std::count_if(
            y_spans->begin(), y_spans->end(),
            [&](uint8_t span_value) { return span_value > coeff_index; });
        if (active_blocks == 0) {
            continue;
        }

        LossyCoeffTable table;
        if (syntax->coefficient_table_bank) {
            table = y_tables[coeff_index];
        } else {
            auto table_read = read_coefficient_table(reader, 1025, "lossy");
            ASSERT_TRUE(table_read.has_value()) << table_read.error().message;
            table = std::move(*table_read);
        }

        const bool use_significance_map = significance_flags[coeff_index] != 0;
        size_t nonzero_count = active_blocks;
        if (use_significance_map) {
            auto presence = read_adaptive_coefficient_presence(reader, active_blocks, "lossy");
            ASSERT_TRUE(presence.has_value()) << presence.error().message;
            nonzero_count = std::count(presence->begin(), presence->end(), static_cast<uint8_t>(1));
            if (nonzero_count == 0) {
                continue;
            }
        }

        const auto single_symbol = syntax->elide_single_symbol_streams
            ? single_symbol_table_value(table, 1025)
            : std::nullopt;
        if (!single_symbol.has_value()) {
            auto encoded_size = reader.read_u32();
            ASSERT_TRUE(encoded_size.has_value()) << encoded_size.error().message;
            auto encoded_bytes = reader.read_bytes(*encoded_size);
            ASSERT_TRUE(encoded_bytes.has_value()) << encoded_bytes.error().message;

            RansDecoder<RANS_PRECISION_BITS> decoder;
            decoder.init(encoded_bytes->data(), encoded_bytes->size());
            ASSERT_TRUE(decoder.ok());

            const size_t symbols_to_decode = use_significance_map ? nonzero_count : active_blocks;
            nonzero_count = 0;
            for (size_t symbol_index = 0; symbol_index < symbols_to_decode; ++symbol_index) {
                const int symbol = decoder.decode(table);
                ASSERT_TRUE(decoder.ok());
                nonzero_count += symbol != 0 ? 1u : 0u;
            }
        } else if (!use_significance_map) {
            nonzero_count = *single_symbol != 0 ? active_blocks : 0u;
        }

        if (nonzero_count == 0) {
            continue;
        }

        if (sign_modes[coeff_index] == kCoefficientSignModeRawPacked) {
            const size_t sign_offset = syntax->stream_offset + reader.position();
            const size_t sign_bytes = packed_coefficient_sign_bytes(nonzero_count);
            ASSERT_GT(payload.size(), sign_offset + sign_bytes - 1);
            if (nonzero_count % 8u != 0u) {
                payload[sign_offset + sign_bytes - 1] |= 0x80;
                corrupted = true;
                break;
            }

            auto sign_bits = read_packed_coefficient_signs(reader, nonzero_count, "lossy");
            ASSERT_TRUE(sign_bits.has_value()) << sign_bits.error().message;
        } else {
            ASSERT_TRUE(sign_modes[coeff_index] == kCoefficientSignModeAllPositive ||
                        sign_modes[coeff_index] == kCoefficientSignModeAllNegative);
        }
    }

    ASSERT_TRUE(corrupted);

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::DecodeFailed);
}

TEST(RoundtripTest, CoefficientSignModeCorruptionRejected) {
    Image image = make_rgb_gradient(24, 24);

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks.front().payload;
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    if (!syntax->adaptive_coefficient_signs) {
        GTEST_SKIP() << "encoded sample did not select adaptive coefficient sign metadata";
    }

    const size_t y_blocks = 9;
    const size_t chroma_blocks = 9;
    const size_t y_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(y_blocks);
    const size_t chroma_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(chroma_blocks);
    const size_t y_span_stream_bytes = encoded_coefficient_span_stream_bytes(
        payload, syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes,
        syntax->adaptive_spans, y_blocks);
    const size_t chroma_span_stream_bytes = encoded_coefficient_span_stream_bytes(
        payload, syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes + y_span_stream_bytes,
        syntax->adaptive_spans, chroma_blocks);
    const size_t plane_extent_bytes = syntax->plane_extents ? 2u : 0u;
    size_t y_extent = 64;
    if (syntax->plane_extents) {
        const size_t y_extent_offset = syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes +
                                       y_span_stream_bytes + chroma_span_stream_bytes;
        ASSERT_GT(payload.size(), y_extent_offset);
        y_extent = payload[y_extent_offset];
    }
    const size_t significance_flag_bytes = syntax->coefficient_significance_maps
        ? packed_coefficient_presence_bytes(y_extent)
        : 0u;
    const size_t sign_mode_offset = syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes +
                                    y_span_stream_bytes + chroma_span_stream_bytes + plane_extent_bytes +
                                    significance_flag_bytes;

    ASSERT_GT(payload.size(), sign_mode_offset);
    payload[sign_mode_offset] = 0xFF;

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::DecodeFailed);
}

TEST(RoundtripTest, PlaneCoefficientExtentCorruptionRejected) {
    Image image(24, 24, BitDepth::Bits8, false);
    for (auto& byte : image.pixels()) {
        byte = 96;
    }

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks.front().payload;
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    const size_t y_blocks = 9;
    const size_t chroma_blocks = 9;
    const size_t y_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(y_blocks);
    const size_t chroma_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(chroma_blocks);
    const size_t y_span_stream_bytes = encoded_coefficient_span_stream_bytes(
        payload, syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes,
        syntax->adaptive_spans, y_blocks);
    const size_t chroma_span_stream_bytes = encoded_coefficient_span_stream_bytes(
        payload, syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes + y_span_stream_bytes,
        syntax->adaptive_spans, chroma_blocks);
    const size_t plane_extent_offset = syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes +
                                       y_span_stream_bytes + chroma_span_stream_bytes;

    ASSERT_TRUE(syntax->plane_extents);
    ASSERT_GT(payload.size(), plane_extent_offset);
    payload[plane_extent_offset] = 0;

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::DecodeFailed);
}

TEST(RoundtripTest, CoefficientTableCorruptionRejected) {
    Image image = make_rgb_gradient(24, 24);

    EncoderConfig config;
    config.quality = 85.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);

    auto& payload = file->tile_chunks.front().payload;
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    const size_t y_blocks = 9;
    const size_t chroma_blocks = 9;
    const size_t y_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(y_blocks);
    const size_t chroma_mode_stream_bytes = sizeof(uint16_t) + packed_prediction_mode_bytes(chroma_blocks);
    const size_t y_span_stream_bytes = encoded_coefficient_span_stream_bytes(
        payload, syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes,
        syntax->adaptive_spans, y_blocks);
    const size_t chroma_span_stream_bytes = encoded_coefficient_span_stream_bytes(
        payload, syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes + y_span_stream_bytes,
        syntax->adaptive_spans, chroma_blocks);
    const size_t plane_extent_bytes = syntax->plane_extents ? 2u : 0u;
    size_t y_extent = 64;
    if (syntax->plane_extents) {
        const size_t y_extent_offset = syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes +
                                       y_span_stream_bytes + chroma_span_stream_bytes;
        ASSERT_GT(payload.size(), y_extent_offset);
        y_extent = payload[y_extent_offset];
    }
    const size_t significance_flag_bytes = syntax->coefficient_significance_maps
        ? packed_coefficient_presence_bytes(y_extent)
        : 0u;
    const size_t sign_mode_bytes = syntax->adaptive_coefficient_signs
        ? packed_coefficient_sign_mode_bytes(y_extent)
        : 0u;
    const size_t coeff_table_offset = syntax->stream_offset + y_mode_stream_bytes + chroma_mode_stream_bytes +
                                      y_span_stream_bytes + chroma_span_stream_bytes + plane_extent_bytes +
                                      significance_flag_bytes + sign_mode_bytes;

    ASSERT_GT(payload.size(), coeff_table_offset);
    payload[coeff_table_offset] = 0xFF;

    auto broken = write_container(*file);
    ASSERT_TRUE(broken.has_value()) << broken.error().message;

    auto decoded = decode(*broken);
    EXPECT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::RansError);
}

TEST(RoundtripTest, PhotoJpegPipelines) {
#ifndef _WIN32
    GTEST_SKIP() << "JPEG integration test requires Windows WIC in this build";
#else
    const std::filesystem::path photo = sample_photo_path();
    if (!std::filesystem::exists(photo)) {
        GTEST_SKIP() << "Sample photo not found: " << photo.string();
    }

    auto source = wk::io::load_image_file(photo.string());
    ASSERT_TRUE(source.has_value()) << source.error().message;
    EXPECT_FALSE(source->has_alpha());

    EncoderConfig lossless_config;
    lossless_config.lossless = true;

    auto encoded_lossless = encode(*source, lossless_config);
    ASSERT_TRUE(encoded_lossless.has_value()) << encoded_lossless.error().message;
    auto decoded_lossless = decode(*encoded_lossless);
    ASSERT_TRUE(decoded_lossless.has_value()) << decoded_lossless.error().message;
    EXPECT_EQ(decoded_lossless->width(), source->width());
    EXPECT_EQ(decoded_lossless->height(), source->height());
    EXPECT_TRUE(std::equal(decoded_lossless->pixels().begin(), decoded_lossless->pixels().end(), source->pixels().begin()));

    EncoderConfig lossy_config;
    lossy_config.quality = 75.0f;

    auto encoded_lossy = encode(*source, lossy_config);
    ASSERT_TRUE(encoded_lossy.has_value()) << encoded_lossy.error().message;
    auto decoded_lossy = decode(*encoded_lossy);
    ASSERT_TRUE(decoded_lossy.has_value()) << decoded_lossy.error().message;
    EXPECT_EQ(decoded_lossy->width(), source->width());
    EXPECT_EQ(decoded_lossy->height(), source->height());

    auto metrics = wk::metrics::compare_images(*source, *decoded_lossy);
    ASSERT_TRUE(metrics.has_value()) << "metrics compare failed: " << metrics.error().message;
    EXPECT_GT(metrics->psnr, 30.0) << "Photo JPEG lossy PSNR too low: " << metrics->psnr;
    EXPECT_GT(metrics->ssim, 0.90) << "Photo JPEG lossy SSIM too low: " << metrics->ssim;
#endif
}

TEST(RoundtripTest, GetInfo) {
    Image image(320, 240, BitDepth::Bits8, true);
    for (auto& byte : image.pixels()) {
        byte = 128;
    }

    EncoderConfig config;
    config.lossless = true;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value());

    auto info = get_info(*encoded);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 320u);
    EXPECT_EQ(info->height, 240u);
    EXPECT_EQ(info->bit_depth, BitDepth::Bits8);
    EXPECT_TRUE(info->is_lossless);
    EXPECT_TRUE(info->has_alpha);
}

TEST(RoundtripTest, EmptyInputError) {
    auto result = decode({});
    EXPECT_FALSE(result.has_value());
}

TEST(RoundtripTest, ZeroDimensionError) {
    Image image(0, 0, BitDepth::Bits8, false);
    auto result = encode(image);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Regression tests for the Critical allocation-safety review (2026-11-13).
// The decoder used to trust three counts straight from the file:
//   * lossless palette count (u16) -> stack array of 256 entries
//   * LZ token count (u32) -> vector allocated before any token is read
//   * LZ backref distance / produced length -> unchecked indices
// and parse_anim() resized its frame vector from a u32 before checking the
// payload size, while parse_fhdr() accepted any non-zero dimension, so a
// tiny header could trigger a multi-GB Image allocation in decode().
// These tests feed the decoder streams no valid encoder would produce and
// require a clean Result error, never a crash, abort, or silent success.
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> build_lossless_palette_file(
    uint16_t palette_count, const std::vector<uint32_t>& colors,
    uint32_t token_count, const std::vector<uint8_t>& token_bytes) {
    ByteWriter payload;
    payload.write_u8(1);  // use_palette
    payload.write_u8(0);  // pred_mode NONE
    payload.write_u16(palette_count);
    for (uint32_t color : colors) {
        payload.write_u32(color);
    }
    payload.write_u32(token_count);
    payload.write_bytes(token_bytes);
    std::vector<uint8_t> payload_bytes = payload.finish();

    TileHeader tile;
    tile.tile_x = 0;
    tile.tile_y = 0;
    tile.layer_flags = TILE_HAS_BASE;
    tile.compressed_size = static_cast<uint32_t>(payload_bytes.size());

    Chunk chunk;
    std::memcpy(chunk.type, CHUNK_TILE, 4);
    chunk.payload = serialize_tile_header(tile);
    chunk.payload.insert(chunk.payload.end(), payload_bytes.begin(),
                         payload_bytes.end());

    WkFile file;
    file.header.width = 8;
    file.header.height = 8;
    file.header.bit_depth = 8;
    file.header.cicp_primaries = 1;
    file.header.cicp_transfer = 1;
    file.header.cicp_matrix = 1;
    file.header.flags =
        FHDR_FLAG_LOSSLESS | FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    file.header.tile_size_log2 = 6;
    file.tile_chunks.push_back(std::move(chunk));

    auto written = write_container(file);
    if (!written.has_value()) {
        return {};
    }
    return *written;
}

std::vector<uint8_t> literal_tokens(size_t count, uint8_t value) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < count; ++i) {
        bytes.push_back(0);  // LITERAL
        bytes.push_back(value);
    }
    return bytes;
}

std::vector<uint8_t> backref_token(uint16_t distance, uint16_t length) {
    ByteWriter writer;
    writer.write_u8(1);  // BACKREFERENCE
    writer.write_u16(distance);
    writer.write_u16(length);
    return writer.finish();
}

}  // namespace

TEST(RoundtripTest, LosslessPaletteSmallImageDecodes) {
    // Positive control: a valid 2-color palette stream must still decode.
    std::vector<uint8_t> tokens;
    for (int k = 0; k < 64; ++k) {
        tokens.push_back(0);  // LITERAL
        tokens.push_back(static_cast<uint8_t>(k % 2));
    }
    auto bytes = build_lossless_palette_file(
        2, {0xFF000000u, 0xFFFFFFFFu}, 64, tokens);
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width(), 8u);
    EXPECT_EQ(decoded->height(), 8u);
    const auto pixels = decoded->pixels();
    EXPECT_EQ(pixels[0], 0u);
    EXPECT_EQ(pixels[1], 0u);
    EXPECT_EQ(pixels[2], 0u);
    EXPECT_EQ(pixels[3], 255u);
    EXPECT_EQ(pixels[4], 255u);
    EXPECT_EQ(pixels[5], 255u);
}

TEST(RoundtripTest, LosslessPaletteCountAbove256Rejected) {
    std::vector<uint32_t> colors(300, 0xFF000000u);
    auto bytes = build_lossless_palette_file(300, colors, 0, {});
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessPaletteCountZeroRejected) {
    auto bytes = build_lossless_palette_file(0, {}, 0, {});
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessPaletteIndexOutOfRangeRejected) {
    // Palette has 1 entry, but every index claims entry 5: undo_palette
    // used to read past the initialized entries.
    auto bytes = build_lossless_palette_file(1, {0xFF000000u}, 64,
                                             literal_tokens(64, 5));
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessLzBackrefDistanceBeyondPositionRejected) {
    // One literal (pos=1), then a backref with distance 5 > pos.
    std::vector<uint8_t> tokens = literal_tokens(1, 0);
    auto backref = backref_token(5, 10);
    tokens.insert(tokens.end(), backref.begin(), backref.end());
    auto bytes = build_lossless_palette_file(1, {0xFF000000u}, 2, tokens);
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessLzBackrefZeroDistanceRejected) {
    std::vector<uint8_t> tokens = literal_tokens(1, 0);
    auto backref = backref_token(0, 3);
    tokens.insert(tokens.end(), backref.begin(), backref.end());
    auto bytes = build_lossless_palette_file(1, {0xFF000000u}, 2, tokens);
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessLzLiteralOverflowRejected) {
    // 65 literals for a 64-pixel image: the 65th would write past indices.
    auto bytes = build_lossless_palette_file(1, {0xFF000000u}, 65,
                                             literal_tokens(65, 0));
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessLzTokenCountAboveRemainingPayloadRejected) {
    auto bytes = build_lossless_palette_file(1, {0xFF000000u}, 0xFFFFFFFFu, {});
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, LosslessLzUnknownTokenTypeRejected) {
    // Type byte 7 is neither LITERAL (0) nor BACKREFERENCE (1).
    // The 4 trailing bytes are the dist/len fields a backref-shaped
    // token carries, so the token reads fully and the rejection must
    // come from the type check, not from truncation.
    auto bytes = build_lossless_palette_file(1, {0xFF000000u}, 1, {7, 0, 0, 0, 0});
    ASSERT_FALSE(bytes.empty());

    auto decoded = decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::LosslessError);
}

TEST(RoundtripTest, HugeAnimationFrameCountDecodeRejected) {
    WkFile file;
    file.header.width = 8;
    file.header.height = 8;
    file.header.bit_depth = 8;
    file.header.cicp_primaries = 1;
    file.header.cicp_transfer = 1;
    file.header.cicp_matrix = 1;
    file.header.flags = FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    file.header.tile_size_log2 = 6;
    file.animation = AnimHeader{};
    file.animation->frame_count = 0xFFFFFFFFu;
    file.animation->loop_count = 0;
    file.animation->background_rgba = 0;
    // frames intentionally empty: 10-byte ANIM payload, huge claim.

    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto decoded = decode(*written);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::AnimationError);
}

TEST(RoundtripTest, CApiDecodeRejectsHugeAnimationWithoutAbort) {
    WkFile file;
    file.header.width = 8;
    file.header.height = 8;
    file.header.bit_depth = 8;
    file.header.cicp_primaries = 1;
    file.header.cicp_transfer = 1;
    file.header.cicp_matrix = 1;
    file.header.flags = FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    file.header.tile_size_log2 = 6;
    file.animation = AnimHeader{};
    file.animation->frame_count = 0xFFFFFFFFu;

    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    uint8_t* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bpp = 0;
    const wk_error_t rc = wk_decode(written->data(), written->size(), &pixels,
                                    &width, &height, &bpp);
    EXPECT_NE(rc, WK_OK);
    EXPECT_EQ(pixels, nullptr);
}

TEST(RoundtripTest, HugeDimensionsDecodeRejected) {
    WkFile file;
    file.header.width = 40000;
    file.header.height = 40000;
    file.header.bit_depth = 8;
    file.header.cicp_primaries = 1;
    file.header.cicp_transfer = 1;
    file.header.cicp_matrix = 1;
    file.header.flags = FHDR_FLAG_TILED | FHDR_FLAG_FULL_RANGE;
    file.header.tile_size_log2 = 9;

    auto written = write_container(file);
    ASSERT_TRUE(written.has_value());

    auto decoded = decode(*written);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code, ErrorCode::InvalidHeader);
}

// ---------------------------------------------------------------------------
// Regression tests for the lossy review findings (2026-11-13, PR3).
//   * The coefficient stream's symbol range is bit-depth aware (see
//     SPEC.md): 8-bit keeps the historical +-1024, 10-bit uses +-4096,
//     12-bit uses +-16384. The in-loop clamp from PR3 now clamps to the
//     same range, so encoder and decoder agree at every bit depth and
//     solid white at quality 100 decodes exactly (12-bit was 2306,
//     10-bit was 642 when the range was +-1024 for all depths).
//   * YUV420 on an image whose first tile is <=8px is undecodable: the
//     decoder infers subsampling from the first tile's chroma block counts
//     and 4:4:4 / 4:2:0 coincide there, so it defaults to 4:4:4.
//   * Quality knob: verified on real photos (ember/mira/horizon) that size
//     and PSNR/SSIM move monotonically with quality; lock that in so a
//     future change cannot silently disconnect the knob from the output.
// ---------------------------------------------------------------------------

namespace {

double mean_abs_error(const Image& a, const Image& b) {
    const auto pa = a.pixels();
    const auto pb = b.pixels();
    double sum = 0.0;
    for (size_t i = 0; i < pa.size(); ++i) {
        sum += std::abs(static_cast<int>(pa[i]) - static_cast<int>(pb[i]));
    }
    return sum / static_cast<double>(pa.size());
}

}  // namespace

TEST(RoundtripTest, Lossy12BitQ100SolidWhiteDecodesExactly) {
    // With a bit-depth-aware symbol range (12-bit: +-16384) the first
    // block's DC coefficient fits the stream, so solid white decodes
    // exactly instead of dark (it was 2306 when the range was +-1024).
    Image image(8, 8, BitDepth::Bits12, false);
    auto pixels = image.pixels();
    for (size_t i = 0; i + 1 < pixels.size(); i += 2) {
        pixels[i] = 0xFF;
        pixels[i + 1] = 0x0F;  // 4095 little-endian
    }

    EncoderConfig config;
    config.quality = 100;
    config.bit_depth = BitDepth::Bits12;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    const auto out = decoded->pixels();
    const int value = static_cast<int>(out[0]) | (static_cast<int>(out[1]) << 8);
    EXPECT_EQ(value, 4095) << "12-bit white decoded as " << value;
}

TEST(RoundtripTest, Lossy10BitQ100SolidWhiteDecodesExactly) {
    // Same at 10-bit (+-4096 range): was 642 with the +-1024 range.
    Image image(8, 8, BitDepth::Bits10, false);
    auto pixels = image.pixels();
    for (size_t i = 0; i + 1 < pixels.size(); i += 2) {
        pixels[i] = 0xFF;
        pixels[i + 1] = 0x03;  // 1023 little-endian
    }

    EncoderConfig config;
    config.quality = 100;
    config.bit_depth = BitDepth::Bits10;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    const auto out = decoded->pixels();
    const int value = static_cast<int>(out[0]) | (static_cast<int>(out[1]) << 8);
    EXPECT_EQ(value, 1023) << "10-bit white decoded as " << value;
}

TEST(RoundtripTest, Lossy12BitQ100SolidWhiteUniformAcrossBlocks) {
    // The in-loop clamp fix (PR3) made the reconstruction climb back to
    // white block by block; with the wider range there is nothing to
    // climb back from -- every block decodes exact white.
    Image image(64, 8, BitDepth::Bits12, false);
    auto pixels = image.pixels();
    for (size_t i = 0; i + 1 < pixels.size(); i += 2) {
        pixels[i] = 0xFF;
        pixels[i + 1] = 0x0F;  // 4095 little-endian
    }

    EncoderConfig config;
    config.quality = 100;
    config.bit_depth = BitDepth::Bits12;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    const auto out = decoded->pixels();
    for (int bx = 0; bx < 8; ++bx) {
        const size_t o = static_cast<size_t>(bx * 8) * 6;
        const int value = static_cast<int>(out[o]) | (static_cast<int>(out[o + 1]) << 8);
        EXPECT_EQ(value, 4095) << "block " << bx << " decoded as " << value;
    }
}

TEST(RoundtripTest, Lossy8BitQ100SolidWhiteStillExact) {
    // Guard: the range change must not touch 8-bit behaviour at all
    // (8-bit keeps the historical +-1024 range).
    Image image(8, 8, BitDepth::Bits8, false);
    auto pixels = image.pixels();
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = 255;
    }

    EncoderConfig config;
    config.quality = 100;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    EXPECT_EQ(decoded->pixels()[0], 255);
}

TEST(RoundtripTest, LossyYuv420TinyImageDecodesCorrectly) {
    Image image(8, 8, BitDepth::Bits8, false);
    auto pixels = image.pixels();
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const size_t o = static_cast<size_t>(y * 8 + x) * 3;
            if (x < 4) {
                pixels[o] = 255;
                pixels[o + 1] = 0;
                pixels[o + 2] = 0;
            } else {
                pixels[o] = 0;
                pixels[o + 1] = 0;
                pixels[o + 2] = 255;
            }
        }
    }

    EncoderConfig config;
    config.quality = 90;
    config.subsampling = Subsampling::YUV420;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    const auto out = decoded->pixels();
    // Left half must stay red, right half blue. Before the fix the file
    // decoded as 4:4:4 and came back with R(3,0)=36, B(7,0)=0.
    EXPECT_GT(out[(0 * 8 + 3) * 3 + 0], 200);
    EXPECT_LT(out[(0 * 8 + 3) * 3 + 2], 50);
    EXPECT_LT(out[(0 * 8 + 7) * 3 + 0], 50);
    EXPECT_GT(out[(0 * 8 + 7) * 3 + 2], 200);
}

TEST(RoundtripTest, LossyYuv420NormalSizeStillWorks) {
    // Guard against the tiny-image fallback leaking into normal sizes:
    // a 64x64 YUV420 encode must still decode close to the original.
    Image image = make_rgb_gradient(64, 64);

    EncoderConfig config;
    config.quality = 90;
    config.subsampling = Subsampling::YUV420;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    EXPECT_LT(mean_abs_error(image, *decoded), 10.0);
}

TEST(RoundtripTest, QualityLowersSizeAndErrorMonotonically) {
    // Photo-like content: a gradient plus deterministic high-frequency
    // texture. (A perfectly smooth gradient alone is not used here: on
    // it, discrete RDO mode choices make the size non-monotonic -- q75
    // can come out smaller than q50 while also being more accurate --
    // which is an encoder tuning quirk, not the knob being disconnected.)
    Image image(128, 128, BitDepth::Bits8, false);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            uint8_t* row = image.row(static_cast<uint32_t>(y)) +
                           static_cast<size_t>(x) * 3;
            const int texture = ((x * 7 + y * 13) % 17) * 3 - 24;
            row[0] = static_cast<uint8_t>(
                std::clamp((x * 255) / 127 + texture, 0, 255));
            row[1] = static_cast<uint8_t>(
                std::clamp((y * 255) / 127 + texture, 0, 255));
            row[2] = static_cast<uint8_t>(
                std::clamp(((x + y) * 255) / 254 + texture, 0, 255));
        }
    }

    std::vector<size_t> sizes;
    std::vector<double> errors;
    for (float quality : {10.0f, 50.0f, 75.0f, 100.0f}) {
        EncoderConfig config;
        config.quality = quality;
        auto encoded = encode(image, config);
        ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
        auto decoded = decode(*encoded);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        sizes.push_back(encoded->size());
        errors.push_back(mean_abs_error(image, *decoded));
    }

    for (size_t i = 1; i < sizes.size(); ++i) {
        EXPECT_GT(sizes[i], sizes[i - 1])
            << "size did not grow with quality at step " << i;
        EXPECT_LT(errors[i], errors[i - 1])
            << "error did not shrink with quality at step " << i;
    }
}

// Regression tests for the PR4 strict-container review (2026-11-13):
// decode() never compared the TILE chunks against the frame grid, so a
// file missing one tile decoded to exit 0 with a blank quadrant, a
// duplicated tile silently overwrote the first copy, and a lossless file
// with extra tiles decoded using only the first one.
TEST(RoundtripTest, DecodeRejectsMissingTile) {
    Image image = make_rgb_gradient(128, 128);

    EncoderConfig config;
    config.quality = 75.0f;
    config.tile_size_log2 = 6;  // 64px tiles -> 2x2 = 4 tiles
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 4u);
    file->tile_chunks.erase(file->tile_chunks.begin() + 2);

    auto rewritten = write_container(*file);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().message;

    auto decoded = decode(*rewritten);
    ASSERT_FALSE(decoded.has_value()) << "decode accepted a file missing one tile";
}

TEST(RoundtripTest, DecodeRejectsDuplicateTile) {
    Image image = make_rgb_gradient(128, 128);

    EncoderConfig config;
    config.quality = 75.0f;
    config.tile_size_log2 = 6;  // 64px tiles -> 2x2 = 4 tiles
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 4u);
    // Keep the chunk count at 4 but cover only 3 grid positions: drop
    // one tile and duplicate another. A bare count check would miss this.
    file->tile_chunks.erase(file->tile_chunks.begin() + 2);
    file->tile_chunks.push_back(file->tile_chunks.front());

    auto rewritten = write_container(*file);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().message;

    auto decoded = decode(*rewritten);
    ASSERT_FALSE(decoded.has_value()) << "decode accepted a file with a duplicated tile";
}

TEST(RoundtripTest, DecodeRejectsExtraTileLossless) {
    Image image = make_rgb_gradient(64, 64);

    EncoderConfig config;
    config.lossless = true;
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 1u);
    file->tile_chunks.push_back(file->tile_chunks.front());

    auto rewritten = write_container(*file);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().message;

    auto decoded = decode(*rewritten);
    ASSERT_FALSE(decoded.has_value()) << "lossless decode accepted a file with an extra tile";
}

TEST(RoundtripTest, DecodeAcceptsRewrittenContainer) {
    // Control: parsing and rewriting a complete file without changing
    // anything must still decode -- the rejections above must come from
    // the missing/duplicated tiles, not from the rewrite itself.
    Image image = make_rgb_gradient(128, 128);

    EncoderConfig config;
    config.quality = 75.0f;
    config.tile_size_log2 = 6;  // 64px tiles -> 2x2 = 4 tiles
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto file = parse_container(*encoded);
    ASSERT_TRUE(file.has_value()) << file.error().message;
    ASSERT_EQ(file->tile_chunks.size(), 4u);

    auto rewritten = write_container(*file);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().message;

    auto decoded = decode(*rewritten);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width(), 128u);
    EXPECT_EQ(decoded->height(), 128u);
}




