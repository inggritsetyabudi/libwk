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
#include <cstring>
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

Image make_solid_image(uint32_t width, uint32_t height, uint8_t value) {
    Image image(width, height, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = value;
            row[x * 3 + 1] = value;
            row[x * 3 + 2] = value;
        }
    }
    return image;
}

Image make_checkerboard(uint32_t width, uint32_t height, uint32_t square) {
    Image image(width, height, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t value = ((x / square) + (y / square)) % 2 == 0 ? 255 : 0;
            row[x * 3 + 0] = value;
            row[x * 3 + 1] = value;
            row[x * 3 + 2] = value;
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
    if (info.layout_tag == kLossyTileLayoutTagV6 || info.layout_tag == kLossyTileLayoutTagV7) {
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
        if (info.layout_tag == kLossyTileLayoutTagV7) {
            // V7 carries two sparse quant-scale streams (luma, then
            // chroma) after the flags byte: count u16, then count
            // entries of (block_index u16, scale_index u8). Skip both
            // so stream_offset lands on the mode streams.
            for (int stream = 0; stream < 2; ++stream) {
                if (payload.size() - info.stream_offset < sizeof(uint16_t)) {
                    return std::unexpected(Error{ErrorCode::TruncatedInput, "lossy payload is missing a V7 scale stream"});
                }
                const uint16_t count = read_le16(payload.data() + info.stream_offset);
                info.stream_offset += sizeof(uint16_t) + static_cast<size_t>(count) * 3;
                if (info.stream_offset > payload.size()) {
                    return std::unexpected(Error{ErrorCode::TruncatedInput, "lossy payload has a truncated V7 scale stream"});
                }
            }
        }
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

// Photo-texture PSNR monotonicity (q90 < q75 anomaly, 2026-11-13).
// A 32x32 crop of a real photo face (mira) reproduces the defect on the
// pre-fix encoder: q75 PSNR 43.33 dB, q80 41.31 dB, q90 48.14 dB --
// higher quality scored WORSE. Root cause: the JPEG base quant table's
// large high-frequency steps (up to 121) resonate with the near-integer
// DCT coefficients of photo blocks, so the rounding error is not
// monotonic in the step size; q75's steps happen to divide the
// coefficients almost exactly (SSE 5.4 on the worst block) while q80's
// do not (SSE 834.8). The fix caps the base table (see QuantTable::build).
// The fixture is the raw RGB bytes of that crop, row-major, 32x32x3.
TEST(RoundtripTest, PhotoTexturePsnrIsMonotonicInQuality) {
    static const char kFace32Rgb[] =
    "\x74\x5e\x50\x91\x79\x6d\x95\x7d\x73\x58\x3c\x33\x85\x69\x60\xa5\x89\x80\x90\x72\x68\x91\x75\x6a\xa5\x85\x79\x9f\x7f\x72\xa8\x8b"
    "\x7d\x9d\x80\x72\xa3\x87\x79\xa7\x8b\x7b\xc7\xae\x9d\xd1\xb8\xa7\xd8\xb4\xa6\xd7\xb3\xa5\xd3\xb0\x9f\xd6\xb3\xa2\xde\xbb\xaa\xdd"
    "\xba\xa9\xdc\xb9\xa6\xdc\xb9\xa6\xdd\xba\xa7\xde\xbb\xa8\xde\xbb\xa6\xe0\xbd\xa8\xe1\xbe\xa9\xde\xbb\xa6\xdb\xb8\xa1\xdc\xb9\xa2"
    "\x74\x5c\x4e\x97\x7f\x73\x8c\x70\x65\x63\x47\x3e\x85\x69\x5e\xa9\x8b\x81\x89\x6b\x61\x98\x7a\x70\xa5\x85\x78\x9f\x7f\x72\xab\x8b"
    "\x7e\x9e\x81\x73\xa6\x89\x7b\xa8\x8c\x7c\xc9\xad\x9d\xd1\xb8\xa7\xd7\xb3\xa5\xd7\xb3\xa5\xd7\xb4\xa3\xd6\xb3\xa2\xda\xb7\xa6\xdd"
    "\xba\xa9\xde\xbb\xa8\xde\xbb\xa8\xdf\xbc\xa9\xe1\xbe\xab\xe2\xbf\xaa\xdf\xbc\xa7\xdc\xb9\xa4\xdd\xba\xa5\xdd\xba\xa3\xde\xbb\xa4"
    "\x74\x5c\x4e\x9d\x83\x78\x7f\x63\x58\x6f\x53\x48\x8c\x70\x65\xab\x8b\x81\x84\x64\x58\xa2\x82\x76\xa6\x84\x77\xa0\x80\x73\xac\x8c"
    "\x7f\xa0\x83\x73\xa8\x8b\x7b\xa9\x8d\x7d\xc9\xad\x9d\xd2\xb6\xa6\xd5\xb1\xa3\xd6\xb2\xa4\xd6\xb3\xa2\xd7\xb4\xa3\xd8\xb5\xa4\xd8"
    "\xb5\xa4\xe1\xbe\xab\xe1\xbe\xab\xe2\xbf\xac\xe0\xbd\xaa\xdd\xba\xa5\xde\xbb\xa6\xde\xbb\xa6\xdf\xbc\xa7\xe0\xbd\xa6\xe0\xbd\xa6"
    "\x77\x5d\x50\x9e\x82\x75\x79\x5d\x52\x76\x5a\x4d\x9c\x7f\x72\xa2\x82\x76\x89\x67\x5b\xa9\x87\x7a\xa6\x84\x75\xa2\x80\x71\xb0\x8e"
    "\x7f\xa5\x85\x76\xab\x8e\x7e\xac\x8f\x7d\xcb\xae\x9c\xd2\xb7\xa4\xd4\xb0\xa2\xd5\xb1\xa3\xd5\xb2\xa1\xd8\xb5\xa4\xd9\xb6\xa5\xda"
    "\xb7\xa6\xdb\xb8\xa5\xdc\xb9\xa6\xde\xbb\xa8\xdf\xbc\xa9\xdf\xbc\xa7\xe0\xbd\xa8\xe1\xbe\xa9\xe0\xbd\xa8\xdf\xbc\xa5\xdf\xbc\xa5"
    "\x7a\x60\x53\x98\x7c\x6f\x77\x5d\x50\x77\x5d\x50\xad\x90\x83\x95\x75\x68\x96\x74\x67\xaa\x86\x78\xa6\x82\x74\xa2\x80\x71\xb2\x90"
    "\x81\xaa\x88\x77\xb0\x91\x7f\xae\x91\x7f\xcb\xae\x9c\xd2\xb5\xa3\xd3\xaf\xa1\xd5\xb1\xa3\xd6\xb3\xa2\xd8\xb5\xa4\xda\xb7\xa6\xdc"
    "\xb9\xa8\xdd\xba\xa7\xde\xbb\xa8\xe0\xbd\xaa\xe1\xbe\xab\xe2\xbf\xaa\xe1\xbe\xa9\xe0\xbd\xa8\xe0\xbd\xa8\xdf\xbc\xa5\xdf\xbc\xa5"
    "\x7e\x62\x55\x92\x78\x6b\x77\x5e\x4f\x82\x66\x58\xb1\x95\x87\x8e\x6e\x61\xa4\x82\x73\xa7\x83\x75\xa6\x83\x72\xa4\x81\x70\xb5\x92"
    "\x81\xad\x8b\x7a\xb5\x93\x82\xb2\x93\x7f\xcd\xae\x9a\xd2\xb5\xa1\xd4\xb0\xa2\xd5\xb1\xa3\xd6\xb3\xa2\xd9\xb6\xa5\xdb\xb8\xa7\xdc"
    "\xb9\xa8\xde\xbb\xa8\xe0\xbd\xaa\xe2\xbf\xac\xe2\xbf\xac\xe1\xbe\xa9\xe1\xbe\xa9\xe0\xbd\xa8\xe0\xbd\xa8\xe0\xbd\xa6\xe0\xbd\xa6"
    "\x7c\x63\x54\x92\x79\x6a\x70\x58\x49\x90\x77\x68\xa9\x8d\x7f\x8f\x72\x62\xae\x8c\x7b\xa6\x80\x70\xa7\x81\x71\xa4\x81\x70\xb6\x93"
    "\x82\xaf\x8d\x7a\xb7\x95\x82\xb3\x94\x80\xce\xaf\x9b\xd3\xb4\xa0\xd4\xb0\xa2\xd6\xb2\xa4\xd7\xb4\xa3\xd9\xb6\xa5\xdb\xb8\xa7\xdd"
    "\xba\xa9\xde\xbb\xa8\xdf\xbc\xa9\xe2\xbf\xac\xe2\xbf\xac\xe1\xbe\xa9\xe1\xbe\xa9\xe1\xbe\xa9\xe0\xbd\xa8\xe0\xbd\xa6\xe0\xbd\xa6"
    "\x7b\x62\x53\x93\x7b\x6c\x69\x51\x42\x9d\x85\x76\x9e\x85\x76\x94\x77\x67\xb2\x90\x7f\xa2\x7f\x6c\xa7\x81\x71\xa6\x80\x70\xb7\x94"
    "\x83\xb2\x8f\x7c\xb8\x96\x83\xb6\x94\x81\xce\xaf\x9b\xd3\xb4\x9e\xd5\xb1\xa3\xd6\xb2\xa4\xd7\xb4\xa3\xd9\xb6\xa5\xdb\xb8\xa7\xdc"
    "\xb9\xa8\xde\xbb\xa8\xdf\xbc\xa9\xe2\xbf\xac\xe2\xbf\xac\xe2\xbf\xaa\xe1\xbe\xa9\xe1\xbe\xa9\xe1\xbe\xa9\xe0\xbd\xa6\xe0\xbd\xa6"
    "\x6c\x5b\x4a\xaf\x9b\x8b\x6c\x58\x48\xa3\x8e\x7c\xa0\x87\x76\x9b\x7e\x6e\xba\x98\x89\xa6\x82\x74\x9d\x7b\x6a\xa6\x84\x73\x9c\x7a"
    "\x67\xae\x8b\x78\xa8\x89\x75\xc7\xa8\x92\xca\xab\x95\xd5\xb6\xa2\xd7\xb3\xa5\xd8\xb4\xa6\xd8\xb5\xa4\xd8\xb5\xa4\xd8\xb5\xa4\xd9"
    "\xb6\xa5\xde\xbb\xa8\xe1\xbe\xab\xde\xbb\xa8\xe0\xbd\xaa\xe2\xbf\xaa\xe4\xc1\xac\xe4\xc1\xac\xe2\xbf\xaa\xe0\xbd\xa6\xde\xbb\xa4"
    "\x63\x58\x47\x9c\x8e\x7d\x62\x54\x43\x9f\x90\x7f\x8c\x78\x68\x9e\x86\x75\xad\x91\x81\xa0\x83\x73\x95\x78\x68\xa4\x82\x73\xaa\x86"
    "\x78\xb2\x8e\x80\xad\x8b\x78\xc5\xa3\x92\xcc\xad\x99\xd3\xb6\xa2\xd6\xb2\xa4\xd6\xb2\xa4\xd7\xb4\xa3\xd8\xb5\xa4\xda\xb7\xa6\xdc"
    "\xb9\xa8\xdc\xb9\xa6\xde\xbb\xa8\xdd\xba\xa7\xdf\xbc\xa9\xe2\xbf\xaa\xe4\xc1\xac\xe4\xc1\xac\xe2\xbf\xaa\xdf\xbc\xa5\xdd\xba\xa3"
    "\x64\x56\x46\x87\x79\x69\x66\x57\x47\x9f\x8e\x7d\x7d\x69\x59\xa6\x8e\x7f\xa5\x89\x7b\xa0\x83\x73\x9e\x7e\x6f\xa1\x81\x72\xae\x91"
    "\x81\xb6\x94\x87\xae\x8c\x7f\xc0\x9e\x91\xcc\xb1\x9e\xd2\xb7\xa2\xd9\xb5\xa7\xd4\xb0\xa2\xd3\xb0\x9f\xd6\xb3\xa2\xdb\xb8\xa7\xdd"
    "\xba\xa9\xdb\xb8\xa5\xda\xb7\xa4\xdd\xba\xa7\xde\xbb\xa8\xe1\xbe\xa9\xe3\xc0\xab\xe3\xc0\xab\xe1\xbe\xa9\xde\xbb\xa4\xdd\xba\xa3"
    "\x5f\x5b\x4b\x6b\x65\x55\x6b\x65\x55\x89\x81\x72\x71\x66\x55\x9b\x8c\x7c\x98\x84\x75\x94\x7e\x70\xac\x92\x85\xa5\x88\x7b\xb0\x90"
    "\x83\xad\x90\x82\xa8\x8c\x7e\xb7\x9a\x8d\xd3\xb3\xa7\xca\xad\xa0\xd4\xb0\xa2\xd5\xb1\xa3\xd1\xae\x9d\xd3\xb0\x9f\xd8\xb5\xa4\xdd"
    "\xba\xa9\xdc\xb9\xa6\xda\xb7\xa4\xdb\xb8\xa5\xdd\xba\xa7\xe0\xbd\xa8\xe2\xbf\xaa\xe2\xbf\xaa\xe0\xbd\xa8\xdd\xba\xa3\xdb\xb8\xa1"
    "\x5f\x55\x4a\x55\x4b\x40\x73\x67\x5d\x6f\x61\x55\x70\x5e\x53\x85\x70\x66\x88\x70\x66\x80\x66\x5d\x87\x67\x5f\x91\x75\x6c\x9d\x83"
    "\x78\xa0\x84\x77\xa1\x84\x76\xb1\x95\x88\xc6\xac\x9f\xbf\xa3\x96\xcc\xa8\x9a\xc8\xa4\x96\xd0\xad\x9c\xcf\xac\x9b\xd4\xb1\xa0\xda"
    "\xb7\xa6\xdd\xba\xa7\xdc\xb9\xa6\xda\xb7\xa4\xdc\xb9\xa6\xdf\xbc\xa7\xe1\xbe\xa9\xe1\xbe\xa9\xdf\xbc\xa7\xdc\xb9\xa2\xda\xb7\xa0"
    "\x4e\x4d\x40\x41\x40\x33\x66\x62\x56\x4c\x46\x38\x5d\x55\x48\x60\x54\x48\x65\x55\x4a\x5d\x4b\x40\x82\x6a\x60\x7c\x60\x57\x9d\x7f"
    "\x77\x94\x7a\x71\x8c\x74\x68\xab\x91\x84\xbd\xa1\x93\xbe\xa2\x94\xca\xa6\x98\xcb\xa7\x99\xd3\xb0\x9f\xcf\xac\x9b\xd2\xaf\x9e\xd7"
    "\xb4\xa3\xdc\xb9\xa6\xdf\xbc\xa9\xd9\xb6\xa3\xdb\xb8\xa5\xde\xbb\xa6\xdf\xbc\xa7\xdf\xbc\xa7\xde\xbb\xa6\xdb\xb8\xa1\xd9\xb6\x9f"
    "\x47\x4b\x3d\x47\x4b\x3d\x59\x5b\x4e\x3f\x3f\x32\x53\x4f\x43\x4f\x47\x3a\x4a\x40\x33\x47\x3b\x2f\x66\x51\x47\x66\x50\x46\x5e\x46"
    "\x3c\x83\x69\x60\x83\x65\x5d\xa2\x88\x7f\xb3\x9d\x91\xb9\x9f\x92\xcb\xa7\x99\xc6\xa2\x94\xc4\xa1\x90\xd3\xb0\x9f\xd2\xaf\x9e\xd5"
    "\xb2\xa1\xda\xb7\xa4\xdf\xbc\xa9\xd8\xb5\xa2\xda\xb7\xa4\xdd\xba\xa5\xdf\xbc\xa7\xdf\xbc\xa7\xdd\xba\xa5\xda\xb7\xa0\xd8\xb5\x9e"
    "\x50\x4e\x46\x5e\x5a\x52\x5c\x58\x4e\x4b\x44\x3b\x56\x4b\x43\x55\x47\x3f\x47\x35\x2c\x4c\x37\x2f\x51\x35\x2c\x66\x4e\x44\x6a\x58"
    "\x4b\x72\x5c\x52\x80\x6a\x60\x90\x76\x6d\xbf\xa1\x99\xb3\x99\x90\xc9\xa5\x97\xc7\xa3\x95\xc3\xa0\x8f\xd7\xb4\xa3\xd4\xb1\xa0\xd4"
    "\xb1\xa0\xd8\xb5\xa2\xdd\xba\xa7\xd8\xb5\xa2\xda\xb7\xa4\xdc\xb9\xa4\xde\xbb\xa6\xde\xbb\xa6\xdc\xb9\xa4\xda\xb7\xa0\xd8\xb5\x9e"
    "\x54\x4d\x3b\x70\x66\x55\x5f\x55\x44\x77\x6d\x5c\x6d\x62\x53\x67\x57\x4c\x5e\x4c\x41\x57\x42\x38\x54\x37\x31\x5f\x4b\x3e\x6b\x55"
    "\x49\x76\x62\x52\x69\x51\x40\x9c\x81\x6e\xa2\x89\x76\x9d\x84\x71\xc1\x9b\x89\xc9\xa3\x91\xc9\xa3\x91\xd9\xb3\xa1\xd5\xaf\x9d\xda"
    "\xb4\xa2\xdb\xb5\xa3\xd8\xb2\xa0\xda\xb4\xa2\xda\xb4\xa2\xd9\xb3\xa1\xd9\xb3\xa1\xd8\xb2\xa0\xd7\xb1\x9f\xd7\xb1\x9f\xd7\xb1\x9f"
    "\x62\x57\x46\x6f\x64\x53\x6b\x60\x51\x86\x78\x6c\x83\x73\x68\x7a\x68\x5f\x7e\x69\x61\x75\x5f\x57\x77\x61\x57\x6f\x55\x4e\x64\x4c"
    "\x40\x74\x5a\x4d\x75\x58\x48\xa2\x87\x74\xa9\x8c\x7a\xa8\x8b\x79\xc5\x9f\x8d\xc9\xa3\x91\xc9\xa3\x91\xd0\xaa\x98\xd9\xb3\xa1\xd8"
    "\xb2\xa0\xd8\xb2\xa0\xd9\xb3\xa1\xda\xb4\xa2\xda\xb4\xa2\xd8\xb2\xa0\xd8\xb2\xa0\xd7\xb1\x9f\xd6\xb0\x9e\xd6\xb0\x9e\xd5\xaf\x9d"
    "\x78\x6a\x5c\x6f\x5f\x56\x7b\x6b\x62\x91\x7f\x76\x8f\x7d\x74\x82\x6c\x64\x93\x7a\x73\x88\x6f\x6c\x83\x69\x64\x7f\x65\x5e\x81\x63"
    "\x59\x92\x70\x64\x87\x67\x58\x9f\x80\x6e\xa8\x86\x75\xb5\x92\x81\xb7\x91\x7f\xc9\xa3\x91\xc8\xa2\x90\xc8\xa2\x90\xdc\xb6\xa4\xd6"
    "\xb0\x9e\xd6\xb0\x9e\xdc\xb6\xa4\xdb\xb5\xa3\xda\xb4\xa2\xd9\xb3\xa1\xd7\xb1\x9f\xd6\xb0\x9e\xd5\xaf\x9d\xd4\xae\x9c\xd4\xae\x9c"
    "\x81\x71\x6a\x65\x55\x4e\x7e\x6c\x65\x8c\x7a\x73\x8e\x79\x73\x7d\x66\x63\x98\x7e\x7b\x8e\x73\x73\x86\x6c\x67\x91\x73\x6d\x9b\x77"
    "\x6e\x9b\x79\x6d\x95\x71\x63\xa6\x80\x70\xad\x86\x74\xba\x93\x81\xbf\x99\x87\xc9\xa3\x91\xc7\xa1\x8f\xcb\xa5\x93\xd5\xaf\x9d\xd8"
    "\xb2\xa0\xd6\xb0\x9e\xde\xb8\xa6\xdb\xb5\xa3\xdb\xb5\xa3\xd8\xb2\xa0\xd7\xb1\x9f\xd4\xae\x9c\xd2\xac\x9a\xd2\xac\x9a\xd1\xab\x99"
    "\x71\x62\x5f\x4f\x40\x3d\x69\x59\x56\x72\x61\x60\x77\x62\x61\x6e\x57\x57\x88\x6f\x6f\x87\x6c\x6c\x93\x76\x74\x9b\x78\x73\x8f\x6f"
    "\x67\x84\x60\x55\x98\x72\x64\xb6\x8f\x7f\xbb\x91\x80\xb8\x8d\x7c\xb9\x93\x81\xb4\x8e\x7c\xc9\xa3\x91\xce\xa8\x96\xcc\xa6\x94\xdb"
    "\xb5\xa3\xd8\xb2\xa0\xde\xb8\xa6\xdb\xb5\xa3\xda\xb4\xa2\xd7\xb1\x9f\xd5\xaf\x9d\xd2\xac\x9a\xcf\xa9\x97\xce\xa8\x96\xcd\xa7\x95"
    "\x59\x4e\x4d\x3b\x30\x2f\x49\x3c\x3c\x49\x3a\x3a\x45\x34\x35\x49\x33\x35\x54\x3d\x3f\x5d\x44\x44\x76\x55\x54\x89\x6b\x67\x8f\x6f"
    "\x67\x82\x5e\x53\x9e\x77\x69\xb6\x8c\x7d\xb7\x8d\x7c\xb3\x8a\x76\xb9\x93\x81\xb3\x8d\x7b\xcc\xa6\x94\xce\xa8\x96\xcf\xa9\x97\xd6"
    "\xb0\x9e\xde\xb8\xa6\xdc\xb6\xa4\xda\xb4\xa2\xd9\xb3\xa1\xd6\xb0\x9e\xd3\xad\x9b\xcf\xa9\x97\xcc\xa6\x94\xca\xa4\x92\xc9\xa3\x91"
    "\x52\x4a\x4d\x3e\x36\x39\x3a\x31\x32\x30\x25\x26\x23\x16\x18\x31\x20\x21\x28\x15\x16\x3a\x24\x26\x4b\x2e\x2c\x6f\x51\x4d\x8c\x6c"
    "\x64\x89\x65\x5a\xa5\x7f\x71\xa9\x82\x70\xa9\x82\x70\xb1\x8a\x76\xb7\x91\x7f\xb6\x90\x7e\xb4\x8e\x7c\xce\xa8\x96\xd0\xaa\x98\xd1"
    "\xab\x99\xe2\xbc\xaa\xd9\xb3\xa1\xd9\xb3\xa1\xd7\xb1\x9f\xd4\xae\x9c\xd0\xaa\x98\xcc\xa6\x94\xc9\xa3\x91\xc7\xa1\x8f\xc5\x9f\x8d"
    "\x57\x52\x54\x4d\x48\x4a\x3e\x39\x3b\x32\x2a\x2d\x25\x1a\x1b\x3a\x2d\x2f\x28\x17\x1a\x41\x2e\x31\x4e\x34\x31\x5c\x3f\x3b\x6e\x4e"
    "\x46\x77\x53\x48\xa8\x82\x74\xaa\x84\x72\xa7\x82\x6d\xb0\x8b\x76\xb5\x8f\x7d\xb9\x93\x81\xb7\x91\x7f\xce\xa8\x96\xd1\xab\x99\xd6"
    "\xb0\x9e\xda\xb4\xa2\xd9\xb3\xa1\xda\xb4\xa2\xd8\xb2\xa0\xd5\xaf\x9d\xd1\xab\x99\xcd\xa7\x95\xc9\xa3\x91\xc7\xa1\x8f\xc5\x9f\x8d"
    "\x5d\x5b\x58\x42\x40\x3d\x9a\x9f\xa1\x97\x98\x9b\x0d\x02\x03\x28\x1b\x1b\x39\x2a\x28\x39\x2a\x28\x52\x3d\x39\x82\x6c\x66\x4a\x30"
    "\x27\x76\x59\x4c\x92\x70\x5f\xa0\x7f\x69\x98\x75\x5e\xb0\x8b\x74\xb2\x8c\x7a\xb5\x8f\x7d\xbc\x96\x84\xc8\xa2\x90\xc9\xa3\x91\xd9"
    "\xb3\xa1\xdb\xb5\xa3\xe2\xbc\xaa\xd9\xb3\xa1\xda\xb4\xa2\xd9\xb3\xa1\xd4\xae\x9c\xcd\xa7\x95\xc5\x9f\x8d\xc1\x9b\x89\xbe\x98\x86"
    "\x40\x42\x3e\x31\x33\x2d\x6d\x6c\x67\x57\x55\x4f\x0f\x0c\x0c\x2d\x26\x24\x43\x36\x36\x69\x5a\x58\x53\x40\x3c\x8b\x76\x70\x58\x42"
    "\x38\x5e\x44\x37\x78\x5b\x49\xa0\x81\x6b\xa9\x8a\x72\xa5\x84\x6c\xb1\x8b\x79\xb4\x8e\x7c\xba\x94\x82\xc7\xa1\x8f\xc8\xa2\x90\xd8"
    "\xb2\xa0\xda\xb4\xa2\xe1\xbb\xa9\xdb\xb5\xa3\xdc\xb6\xa4\xdc\xb6\xa4\xd7\xb1\x9f\xcf\xa9\x97\xc7\xa1\x8f\xc2\x9c\x8a\xbf\x99\x87"
    "\x1c\x22\x1b\x3d\x40\x3a\x1d\x1f\x18\x1e\x1d\x17\x28\x26\x20\x22\x1b\x14\x47\x3c\x3b\x4a\x3b\x3b\x71\x61\x5a\x95\x83\x7a\x6f\x5b"
    "\x4e\x5b\x43\x34\x6c\x53\x42\x93\x78\x63\xb0\x95\x7e\x9d\x80\x6a\xb0\x8a\x78\xb2\x8c\x7a\xb9\x93\x81\xc5\x9f\x8d\xc7\xa1\x8f\xd6"
    "\xb0\x9e\xd8\xb2\xa0\xdf\xb9\xa7\xe1\xbb\xa9\xde\xb8\xa6\xde\xb8\xa6\xda\xb4\xa2\xd2\xac\x9a\xc9\xa3\x91\xc1\x9b\x89\xbe\x98\x86"
    "\x3d\x3f\x38\x5c\x5e\x57\x2c\x2c\x23\x1a\x16\x0e\x4a\x43\x3c\x2d\x22\x1c\x3e\x32\x2c\x4d\x41\x3b\x84\x72\x6b\x7e\x6c\x63\x6f\x59"
    "\x4d\x65\x4d\x3e\x74\x5b\x4a\x7a\x61\x4c\xa4\x89\x72\xa0\x85\x6e\xb0\x8a\x78\xb2\x8c\x7a\xb9\x93\x81\xc5\x9f\x8d\xc7\xa1\x8f\xd6"
    "\xb0\x9e\xd8\xb2\xa0\xdf\xb9\xa7\xe2\xbc\xaa\xdf\xb9\xa7\xe0\xba\xa8\xdd\xb7\xa5\xd4\xae\x9c\xca\xa4\x92\xc2\x9c\x8a\xbd\x97\x85"
    "\x46\x3f\x36\x60\x59\x50\x41\x39\x30\x36\x2a\x22\x2a\x1c\x14\x3d\x2b\x24\x41\x2c\x26\x52\x3d\x37\x76\x5d\x58\x61\x48\x41\x6c\x50"
    "\x45\x6f\x52\x44\x7f\x62\x52\x77\x5a\x46\xa1\x82\x6c\xa7\x88\x72\xb2\x8c\x7a\xb5\x8f\x7d\xbb\x95\x83\xc8\xa2\x90\xc9\xa3\x91\xd9"
    "\xb3\xa1\xda\xb4\xa2\xe1\xbb\xa9\xec\xc6\xb4\xe5\xbf\xad\xe1\xbb\xa9\xde\xb8\xa6\xd6\xb0\x9e\xcb\xa5\x93\xc2\x9c\x8a\xbc\x96\x84"
    "\x52\x44\x38\x79\x69\x5e\x8a\x78\x6d\x7b\x66\x5c\x6b\x55\x4d\x76\x5d\x56\x84\x6a\x65\x83\x66\x62\x75\x57\x51\x75\x55\x4d\x8a\x67"
    "\x5e\x87\x63\x57\x94\x70\x62\x94\x71\x5e\xae\x8b\x78\xad\x8a\x77\xb7\x91\x7f\xba\x94\x82\xc0\x9a\x88\xcd\xa7\x95\xce\xa8\x96\xde"
    "\xb8\xa6\xe0\xba\xa8\xe7\xc1\xaf\xec\xc6\xb4\xea\xc4\xb2\xdf\xb9\xa7\xde\xb8\xa6\xd7\xb1\x9f\xcc\xa6\x94\xc3\x9d\x8b\xbc\x96\x84"
    "\x56\x40\x32\x84\x6e\x60\xa0\x88\x7a\x95\x7b\x70\x9f\x83\x7a\x98\x7a\x72\x8b\x6d\x67\x93\x72\x6d\x86\x63\x5e\x91\x6d\x66\xa4\x80"
    "\x77\x9e\x78\x6c\xa5\x7f\x71\xaa\x84\x72\xb2\x8c\x7a\xb4\x8e\x7c\xbb\x95\x83\xbe\x98\x86\xc4\x9e\x8c\xd1\xab\x99\xd2\xac\x9a\xe2"
    "\xbc\xaa\xe3\xbd\xab\xea\xc4\xb2\xea\xc4\xb2\xed\xc7\xb5\xee\xc8\xb6\xdd\xb7\xa5\xd7\xb1\x9f\xcd\xa7\x95\xc4\x9e\x8c\xbd\x97\x85"
    "\x69\x50\x41\x97\x7e\x6f\x9d\x81\x73\x8a\x6e\x61\xb3\x95\x8b\x87\x69\x61\x9b\x7b\x73\x8c\x6c\x64\x84\x60\x5b\x8c\x68\x61\x9e\x77"
    "\x6f\xa3\x7b\x70\xa9\x82\x74\xa8\x82\x70\xa7\x81\x6f\xbb\x95\x83\xbd\x97\x85\xc0\x9a\x88\xc6\xa0\x8e\xd3\xad\x9b\xd4\xae\x9c\xe4"
    "\xbe\xac\xe6\xc0\xae\xed\xc7\xb5\xe8\xc2\xb0\xec\xc6\xb4\xef\xc9\xb7\xdd\xb7\xa5\xd7\xb1\x9f\xcf\xa9\x97\xc5\x9f\x8d\xbe\x98\x86";
    static_assert(sizeof(kFace32Rgb) - 1 == 32u * 32u * 3u, "face fixture size");
    Image image(32, 32, BitDepth::Bits8, false);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(kFace32Rgb);
    for (uint32_t y = 0; y < 32; ++y) {
        uint8_t* row = image.row(y);
        std::memcpy(row, src + y * 32 * 3, 32 * 3);
    }

    double psnr_at[4] = {};
    size_t size_at[4] = {};
    const float qualities[4] = {50.0f, 75.0f, 80.0f, 90.0f};
    for (int i = 0; i < 4; ++i) {
        EncoderConfig config;
        config.quality = qualities[i];
        config.subsampling = Subsampling::YUV444;
        auto encoded = encode(image, config);
        ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
        auto decoded = decode(*encoded);
        ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
        auto metrics = wk::metrics::compare_images(image, *decoded);
        ASSERT_TRUE(metrics.has_value()) << "metrics compare failed: " << metrics.error().message;
        psnr_at[i] = metrics->psnr;
        size_at[i] = encoded->size();
    }
    // Strict: higher quality must not score lower, up to 0.05 dB of float
    // noise. (The pre-fix encoder regressed 2.02 dB from q75 to q80 here.)
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(psnr_at[i + 1], psnr_at[i] - 0.05)
            << "PSNR regressed from q" << qualities[i] << " (" << psnr_at[i]
            << " dB) to q" << qualities[i + 1] << " (" << psnr_at[i + 1] << " dB)";
        EXPECT_GE(size_at[i + 1], size_at[i])
            << "size regressed from q" << qualities[i] << " to q" << qualities[i + 1];
    }
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

TEST(RoundtripTest, SmoothChromaRampKeepsFineStepsAtQ75) {
    // A smooth color ramp (green -> gray -> green). Without the
    // content-adaptive chroma refinement the decoded ramp collapses into
    // visible bands (84 distinct R levels, steps up to 5 at q75).
    Image image(256, 256, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < 256; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 256; ++x) {
            uint8_t r = static_cast<uint8_t>(x);
            uint8_t g = static_cast<uint8_t>(255 - x);
            uint8_t b = 128;
            if (x >= 128) {
                r = static_cast<uint8_t>(255 - r);
                g = static_cast<uint8_t>(255 - g);
                b = static_cast<uint8_t>(255 - b);
            }
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
    }

    EncoderConfig config;
    config.quality = 75.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    const uint8_t* drow = decoded->row(128);
    bool seen[256] = {};
    int distinct = 0;
    int max_step = 0;
    int prev = -1;
    for (uint32_t x = 0; x < 128; ++x) {
        const int v = drow[x * 3 + 0];
        if (!seen[v]) {
            seen[v] = true;
            ++distinct;
        }
        if (prev >= 0) {
            max_step = std::max(max_step, std::abs(v - prev));
        }
        prev = v;
    }
    EXPECT_GE(distinct, 90) << "smooth ramp lost too many levels (banding)";
    EXPECT_LE(max_step, 2) << "smooth ramp has visible steps";
}

// The q50 version of the ramp above. At q50 the seed row (first row
// and column, which have no above/left reference) is the bottleneck:
// its block means sit on a 2-level grid (luma DC step 16) and every
// interior block copies the row above, extruding the seed's staircase
// down the tile. The V7 seed boost gives the seed blocks quant scale
// 0.75, which puts their means on a finer grid. Measured: distinct
// 80 -> 93 (the colorspace floor), worst step 4 -> 3, only one step
// larger than 2, at essentially the same file size. The thresholds
// below are the measured result, not a loosened bar -- the pre-boost
// q50 (80 distinct, worst step 4) FAILS both of them.
TEST(RoundtripTest, SeedBoostFixesRampAtQ50) {
    Image image(256, 256, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < 256; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 256; ++x) {
            uint8_t r = static_cast<uint8_t>(x);
            uint8_t g = static_cast<uint8_t>(255 - x);
            uint8_t b = 128;
            if (x >= 128) {
                r = static_cast<uint8_t>(255 - r);
                g = static_cast<uint8_t>(255 - g);
                b = static_cast<uint8_t>(255 - b);
            }
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
    }

    EncoderConfig config;
    config.quality = 50.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

    // The seed boost is a V7 feature: the file must be V7, and the
    // scale stream must actually carry the seed entries (not empty).
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_EQ(parsed->tile_chunks.size(), 1u);
    const auto& payload = parsed->tile_chunks[0].payload;
    ASSERT_GE(payload.size(), 277u);
    const uint32_t tag = static_cast<uint32_t>(payload[273]) |
                         (static_cast<uint32_t>(payload[274]) << 8) |
                         (static_cast<uint32_t>(payload[275]) << 16) |
                         (static_cast<uint32_t>(payload[276]) << 24);
    EXPECT_EQ(tag, kLossyTileLayoutTagV7) << "seed boost must emit V7";

    const uint8_t* drow = decoded->row(128);
    bool seen[256] = {};
    int distinct = 0;
    int max_step = 0;
    int prev = -1;
    for (uint32_t x = 0; x < 128; ++x) {
        const int v = drow[x * 3 + 0];
        if (!seen[v]) {
            seen[v] = true;
            ++distinct;
        }
        if (prev >= 0) {
            max_step = std::max(max_step, std::abs(v - prev));
        }
        prev = v;
    }
    EXPECT_GE(distinct, 90) << "q50 ramp lost too many levels (banding)";
    EXPECT_LE(max_step, 3) << "q50 ramp has visible steps";
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
    // A 64x64 sample (64 luma blocks) so that some coefficient's
    // RawPacked sign stream has a nonzero count that is not a multiple
    // of 8 -- the padding bit the corruption flips. A 24x24 sample
    // (9 blocks) stopped yielding one after the quantization-aware
    // RDO changed the coefficient distribution.
    Image image = make_rgb_gradient(64, 64);

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

    const size_t y_blocks = 64;
    const size_t chroma_blocks = 64;
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





// Layout V7 is emitted only when a block actually uses it: a
// non-identity quant scale, or the PLANAR prediction mode (13). Content
// where planar cannot win (a solid block: DC ties planar and wins by
// mode order; a checkerboard: V predicts it exactly) stays on V6, and
// a smooth gradient that does use planar is V7 with a planar mode in
// its mode stream. This replaced the old "textured scene stays V6"
// guard: with the quantization-aware RDO, planar legitimately wins on
// smooth regions of textured scenes and even on noise (averaging the
// noisy references lowers the residual variance), so no natural scene
// is guaranteed V6.
TEST(RoundtripTest, V7OnlyWhenPlanarOrScaleUsed) {
    EncoderConfig config;
    config.quality = 75.0f;
    config.subsampling = Subsampling::YUV444;

    auto layout_tag_of = [](const Image& image, const EncoderConfig& cfg) {
        auto encoded = encode(image, cfg);
        EXPECT_TRUE(encoded.has_value()) << encoded.error().message;
        if (!encoded.has_value()) {
            return 0u;
        }
        auto parsed = parse_container(*encoded);
        EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
        if (!parsed.has_value() || parsed->tile_chunks.empty()) {
            return 0u;
        }
        const auto& payload = parsed->tile_chunks[0].payload;
        EXPECT_GE(payload.size(), 277u);
        if (payload.size() < 277u) {
            return 0u;
        }
        return static_cast<uint32_t>(payload[273]) |
               (static_cast<uint32_t>(payload[274]) << 8) |
               (static_cast<uint32_t>(payload[275]) << 16) |
               (static_cast<uint32_t>(payload[276]) << 24);
    };

    EXPECT_EQ(layout_tag_of(make_solid_image(64, 64, 128), config),
              kLossyTileLayoutTagV6)
        << "solid image must stay on V6";
    EXPECT_EQ(layout_tag_of(make_checkerboard(64, 64, 8), config),
              kLossyTileLayoutTagV6)
        << "checkerboard must stay on V6";

    // A smooth gradient uses planar and must be V7 -- and the V7 tag
    // must be truthful: a planar mode is actually in the mode stream.
    Image gradient = make_rgb_gradient(64, 64);
    auto encoded = encode(gradient, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_EQ(parsed->tile_chunks.size(), 1u);
    const auto& payload = parsed->tile_chunks[0].payload;
    ASSERT_GE(payload.size(), 277u);
    const uint32_t tag = static_cast<uint32_t>(payload[273]) |
                         (static_cast<uint32_t>(payload[274]) << 8) |
                         (static_cast<uint32_t>(payload[275]) << 16) |
                         (static_cast<uint32_t>(payload[276]) << 24);
    ASSERT_EQ(tag, kLossyTileLayoutTagV7) << "gradient must use V7";
    auto syntax = parse_lossy_tile_syntax(payload);
    ASSERT_TRUE(syntax.has_value()) << syntax.error().message;
    ByteReader reader(std::span<const uint8_t>(payload.data() + syntax->stream_offset,
                                               payload.size() - syntax->stream_offset));
    auto y_modes = read_packed_prediction_modes(reader, 64, "luma");
    ASSERT_TRUE(y_modes.has_value()) << y_modes.error().message;
    auto chroma_modes = read_packed_prediction_modes(reader, 64, "chroma");
    ASSERT_TRUE(chroma_modes.has_value()) << chroma_modes.error().message;
    const bool any_planar =
        std::any_of(y_modes->begin(), y_modes->end(),
                    [](PredMode m) { return m == PredMode::PLANAR; }) ||
        std::any_of(chroma_modes->begin(), chroma_modes->end(),
                    [](PredMode m) { return m == PredMode::PLANAR; });
    EXPECT_TRUE(any_planar)
        << "V7 tag must mean a planar mode is actually used";
}

// Layout V7 decoder path, exercised by byte surgery: take a V6 file,
// insert sparse quant-scale streams after the syntax-flags byte and
// retag it V7. The scale streams sit at tile-payload offset 278
// (9-byte tile header + 256 quant-table bytes + 8 block-dim bytes
// + 4 layout-tag bytes + 1 flags byte). The fixture is a checkerboard:
// it stays V6 (V predicts it exactly, so planar never wins) while
// still carrying real coefficients.
TEST(RoundtripTest, V7EmptyScaleListsDecodeIdenticalToV6) {
    Image image = make_checkerboard(64, 64, 8);

    EncoderConfig config;
    config.quality = 75.0f;
    config.subsampling = Subsampling::YUV444;

    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_EQ(parsed->tile_chunks.size(), 1u);
    auto payload = parsed->tile_chunks[0].payload;
    ASSERT_GE(payload.size(), 278u);
    const uint32_t tag = static_cast<uint32_t>(payload[273]) |
                         (static_cast<uint32_t>(payload[274]) << 8) |
                         (static_cast<uint32_t>(payload[275]) << 16) |
                         (static_cast<uint32_t>(payload[276]) << 24);
    ASSERT_EQ(tag, kLossyTileLayoutTagV6) << "test fixture must start on V6";

    // Two empty scale lists: count 0 for luma, count 0 for chroma.
    const std::vector<uint8_t> scales = {0, 0, 0, 0};
    payload.insert(payload.begin() + 278, scales.begin(), scales.end());
    const uint32_t v7 = kLossyTileLayoutTagV7;
    payload[273] = static_cast<uint8_t>(v7 & 0xFF);
    payload[274] = static_cast<uint8_t>((v7 >> 8) & 0xFF);
    payload[275] = static_cast<uint8_t>((v7 >> 16) & 0xFF);
    payload[276] = static_cast<uint8_t>((v7 >> 24) & 0xFF);
    uint32_t compressed_size = static_cast<uint32_t>(payload[5]) |
                               (static_cast<uint32_t>(payload[6]) << 8) |
                               (static_cast<uint32_t>(payload[7]) << 16) |
                               (static_cast<uint32_t>(payload[8]) << 24);
    compressed_size += static_cast<uint32_t>(scales.size());
    payload[5] = static_cast<uint8_t>(compressed_size & 0xFF);
    payload[6] = static_cast<uint8_t>((compressed_size >> 8) & 0xFF);
    payload[7] = static_cast<uint8_t>((compressed_size >> 16) & 0xFF);
    payload[8] = static_cast<uint8_t>((compressed_size >> 24) & 0xFF);
    parsed->tile_chunks[0].payload = payload;

    auto patched = write_container(*parsed);
    ASSERT_TRUE(patched.has_value()) << patched.error().message;

    auto decoded_v6 = decode(*encoded);
    ASSERT_TRUE(decoded_v6.has_value()) << decoded_v6.error().message;
    auto decoded_v7 = decode(*patched);
    ASSERT_TRUE(decoded_v7.has_value()) << decoded_v7.error().message;

    // Empty scale lists mean every block uses scale 1.0, so the V7
    // decode must be pixel-identical to the V6 decode.
    EXPECT_EQ(decoded_v7->pixels().size(), decoded_v6->pixels().size());
    EXPECT_TRUE(std::equal(decoded_v7->pixels().begin(), decoded_v7->pixels().end(),
                           decoded_v6->pixels().begin()))
        << "V7 with identity scales must decode exactly like V6";
}
