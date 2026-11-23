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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include "../src/core/image_io.h"
#include "../src/core/avif_decode.h"
#include "../src/core/jxl_decode.h"

using namespace wk;

namespace {

std::filesystem::path write_temp_ppm(const std::string& name, const std::string& header,
                                     const std::vector<uint8_t>& payload) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file << header;
    file.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    file.close();
    return path;
}

void remove_temp(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

TEST(ImageIoTest, LoadPpmRejectsHugeWidth) {
    // Before the fix, std::stoul on a 26-digit width threw out_of_range
    // out of load_ppm with no handler: `wkenc` aborted (exit 134).
    auto path = write_temp_ppm("wk_huge_width.ppm", "P6\n99999999999999999999999999 2\n255\n",
                               std::vector<uint8_t>(18, 0));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadPpmRejectsNegativeWidth) {
    // "-5" is not a dimension: stoul used to wrap it to ULONG_MAX and the
    // truncation to uint32 only failed later on the payload-size check.
    auto path = write_temp_ppm("wk_neg_width.ppm", "P6\n-5 2\n255\n",
                               std::vector<uint8_t>(18, 0));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadPpmRejectsHugeMaxValue) {
    auto path = write_temp_ppm("wk_huge_max.ppm", "P6\n2 2\n99999999999999999999\n",
                               std::vector<uint8_t>(12, 0));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadPpmRejectsZeroDimensions) {
    auto path = write_temp_ppm("wk_zero_dim.ppm", "P6\n0 2\n255\n", {});
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadPpmRejectsNonNumericHeader) {
    auto path = write_temp_ppm("wk_alpha_dim.ppm", "P6\nab 2\n255\n",
                               std::vector<uint8_t>(12, 0));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadPpmAcceptsValid) {
    // Control: a well-formed 2x2 PPM still loads with the right pixels.
    std::vector<uint8_t> payload = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    auto path = write_temp_ppm("wk_valid.ppm", "P6\n2 2\n255\n", payload);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 2u);
    EXPECT_EQ(loaded->height(), 2u);
    EXPECT_EQ(loaded->pixels().size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), loaded->pixels().begin()));
}

TEST(ImageIoTest, LoadPpmKeepsLeadingWhitespaceRasterByte) {
    // Regression: load_ppm skipped ALL whitespace after maxval, so a
    // first raster byte that is itself whitespace (0x09 tab here) was
    // eaten and the payload-size check rejected the file. The raster
    // follows a single whitespace delimiter after maxval.
    std::vector<uint8_t> payload = {0x09, 0x76, 0xaf, 0x09, 0x76, 0xaf,
                                    0x09, 0x76, 0xaf, 0x09, 0x76, 0xaf};
    auto path = write_temp_ppm("wk_ws_raster.ppm", "P6\n2 2\n255\n", payload);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 2u);
    EXPECT_EQ(loaded->height(), 2u);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), loaded->pixels().begin()));
}

TEST(ImageIoTest, LoadPpmRejectsTruncatedPayload) {
    // Control: the pre-existing payload-size check still works.
    auto path = write_temp_ppm("wk_short.ppm", "P6\n2 2\n255\n",
                               std::vector<uint8_t>(6, 0));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

std::filesystem::path photos_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "photos";
}

std::array<uint8_t, 3> pixel_rgb(const Image& image, uint32_t x, uint32_t y) {
    const size_t bpp = image.has_alpha() ? 4u : 3u;
    const size_t offset = (static_cast<size_t>(y) * image.width() + x) * bpp;
    const auto pixels = image.pixels();
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}

TEST(ImageIoTest, LoadJpgLoadsRealPhoto) {
    // The Termux gap: JPG input used to fail on Linux ("unsupported
    // input image format") because only Windows/WIC could read it.
    // stb_image decodes it on every platform. JPEG decoders differ by
    // a unit or two per channel, so pixels are checked within +-3 of
    // the reference decode, not exactly.
    auto loaded = io::load_image_file((photos_root() / "people" / "ember-7f3a.jpg").string());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 225u);
    EXPECT_EQ(loaded->height(), 225u);
    EXPECT_FALSE(loaded->has_alpha());
    const auto check = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
        const auto pixel = pixel_rgb(*loaded, x, y);
        EXPECT_LE(std::abs(static_cast<int>(pixel[0]) - r), 3);
        EXPECT_LE(std::abs(static_cast<int>(pixel[1]) - g), 3);
        EXPECT_LE(std::abs(static_cast<int>(pixel[2]) - b), 3);
    };
    check(0, 0, 42, 51, 84);
    check(112, 112, 216, 175, 121);
    check(224, 224, 213, 189, 165);
}

TEST(ImageIoTest, LoadPngMatchesKnownPixelsExactly) {
    // PNG is lossless, so stb_image must return exactly the reference
    // pixels (no decoder tolerance, unlike JPG).
    auto loaded = io::load_image_file((photos_root() / "people" / "ember-7f3a.png").string());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 225u);
    EXPECT_EQ(loaded->height(), 225u);
    EXPECT_FALSE(loaded->has_alpha());
    EXPECT_EQ(pixel_rgb(*loaded, 0, 0), (std::array<uint8_t, 3>{43, 51, 84}));
    EXPECT_EQ(pixel_rgb(*loaded, 112, 112), (std::array<uint8_t, 3>{214, 183, 131}));
    EXPECT_EQ(pixel_rgb(*loaded, 224, 224), (std::array<uint8_t, 3>{135, 199, 83}));
    EXPECT_EQ(pixel_rgb(*loaded, 50, 200), (std::array<uint8_t, 3>{212, 197, 178}));
    EXPECT_EQ(pixel_rgb(*loaded, 200, 50), (std::array<uint8_t, 3>{158, 138, 108}));
}

TEST(ImageIoTest, SavePngRoundtripRgbAndRgba) {
    // PNG output used to fail on Linux too (Windows/WIC only).
    const std::vector<uint8_t> rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    Image rgb_image(2, 2, BitDepth::Bits8, false);
    std::copy(rgb.begin(), rgb.end(), rgb_image.pixels().begin());
    auto rgb_path = std::filesystem::temp_directory_path() / "wk_rgb.png";
    ASSERT_TRUE(io::save_image_file(rgb_path.string(), rgb_image).has_value());
    auto rgb_loaded = io::load_image_file(rgb_path.string());
    remove_temp(rgb_path);
    ASSERT_TRUE(rgb_loaded.has_value()) << rgb_loaded.error().message;
    EXPECT_EQ(rgb_loaded->pixels().size(), rgb.size());
    EXPECT_TRUE(std::equal(rgb.begin(), rgb.end(), rgb_loaded->pixels().begin()));

    const std::vector<uint8_t> rgba = {255, 0, 0, 128, 0, 255, 0, 255,
                                       0, 0, 255, 64,  255, 255, 255, 0};
    Image rgba_image(2, 2, BitDepth::Bits8, true);
    std::copy(rgba.begin(), rgba.end(), rgba_image.pixels().begin());
    auto rgba_path = std::filesystem::temp_directory_path() / "wk_rgba.png";
    ASSERT_TRUE(io::save_image_file(rgba_path.string(), rgba_image).has_value());
    auto rgba_loaded = io::load_image_file(rgba_path.string());
    remove_temp(rgba_path);
    ASSERT_TRUE(rgba_loaded.has_value()) << rgba_loaded.error().message;
    EXPECT_TRUE(rgba_loaded->has_alpha());
    EXPECT_EQ(rgba_loaded->pixels().size(), rgba.size());
    EXPECT_TRUE(std::equal(rgba.begin(), rgba.end(), rgba_loaded->pixels().begin()));
}

TEST(ImageIoTest, LoadRejectsGarbageJpg) {
    auto path = std::filesystem::temp_directory_path() / "wk_garbage.jpg";
    {
        std::ofstream file(path, std::ios::binary);
        file << "this is not a jpeg file at all, just text";
    }
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadRejectsUnsupportedExtension) {
    auto path = std::filesystem::temp_directory_path() / "wk_unsupported.tiff";
    {
        std::ofstream file(path, std::ios::binary);
        file << "II*\0";
    }
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

std::filesystem::path write_temp_bytes(const std::string& name,
                                       const std::vector<uint8_t>& bytes) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    return path;
}

std::vector<uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

// Tiny 4x4 lossless WebP (PIL, lossless=True), embedded as bytes so the
// test needs no binary fixture file (snapshot pushes are UTF-8 only).
// Row-major pixels:
//   (255,0,0) (0,255,0) (0,0,255) (255,255,0)
//   (0,255,255) (255,0,255) (128,128,128) (0,0,0)
//   (255,255,255) (10,20,30) (40,50,60) (70,80,90)
//   (1,2,3) (4,5,6) (7,8,9) (250,240,230)
const std::vector<uint8_t> kTinyLosslessWebp = {
    0x52, 0x49, 0x46, 0x46, 0x60, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x4c, 0x54, 0x00, 0x00, 0x00, 0x2f, 0x03, 0xc0, 0x00,
    0x00, 0x7f, 0x60, 0xa6, 0x6d, 0x9b, 0x4c, 0x79, 0x87, 0xa1, 0x18, 0x06,
    0x6d, 0xfc, 0x9f, 0xd2, 0x30, 0x90, 0xb6, 0x4d, 0xf8, 0x7c, 0x4c, 0xc8,
    0xa5, 0xdd, 0xfc, 0xcd, 0xb4, 0x91, 0x72, 0x0f, 0x58, 0x60, 0xfd, 0x11,
    0x69, 0xf8, 0x77, 0xf0, 0xf3, 0x1f, 0x20, 0xa2, 0x00, 0xe1, 0x31, 0x80,
    0x28, 0x38, 0xc3, 0x34, 0xd3, 0x36, 0x2b, 0xb5, 0xd4, 0x6e, 0xcf, 0xdf,
    0x03, 0x41, 0x00, 0xc4, 0x81, 0x1b, 0x01, 0x56, 0x54, 0x39, 0x31, 0xa3,
    0x44, 0x49, 0x44, 0xff, 0x83, 0x32, 0xba, 0x1c};

const std::vector<std::array<uint8_t, 3>> kTinyLosslessPixels = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
    {0, 255, 255}, {255, 0, 255}, {128, 128, 128}, {0, 0, 0},
    {255, 255, 255}, {10, 20, 30}, {40, 50, 60}, {70, 80, 90},
    {1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {250, 240, 230}};

// Animated WebP (2-frame 64x64, PIL; fuzz corpus anim.webp), embedded as
// bytes so the test needs no binary fixture file (snapshot pushes are
// UTF-8 only). Chunks: VP8X (animation flag) + ANIM + 2x ANMF.
const std::vector<uint8_t> kAnimatedWebp = {
    0x52, 0x49, 0x46, 0x46, 0xf0, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x3f, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x41, 0x4e, 0x49, 0x4d, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46,
    0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00,
    0x00, 0x3f, 0x00, 0x00, 0x64, 0x00, 0x00, 0x02, 0x56, 0x50, 0x38, 0x20,
    0x4e, 0x00, 0x00, 0x00, 0xb0, 0x04, 0x00, 0x9d, 0x01, 0x2a, 0x40, 0x00,
    0x40, 0x00, 0x3e, 0x6d, 0x36, 0x99, 0x49, 0x24, 0x23, 0x22, 0xa1, 0x22,
    0x08, 0x00, 0x80, 0x0d, 0x89, 0x69, 0x06, 0x28, 0x07, 0xef, 0xf8, 0x9d,
    0x00, 0x07, 0xf1, 0xae, 0xf4, 0x89, 0xc6, 0x4a, 0xdc, 0x32, 0x13, 0xf1,
    0x80, 0x00, 0x00, 0xfe, 0xf0, 0x1b, 0xdf, 0xff, 0xa1, 0x46, 0xca, 0x26,
    0xbc, 0x7f, 0xff, 0xed, 0x2c, 0xff, 0xea, 0x59, 0xff, 0xd4, 0xb3, 0xfb,
    0xf2, 0x7f, 0x88, 0x11, 0xfb, 0xc4, 0x7f, 0x4c, 0x80, 0x00, 0x41, 0x4e,
    0x4d, 0x46, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3f, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x56, 0x50,
    0x38, 0x20, 0x3e, 0x00, 0x00, 0x00, 0x94, 0x03, 0x00, 0x9d, 0x01, 0x2a,
    0x40, 0x00, 0x40, 0x00, 0x3e, 0x6d, 0x32, 0x92, 0x49, 0x02, 0x20, 0x00,
    0x00, 0xd8, 0x96, 0x90, 0x07, 0x60, 0x00, 0x3f, 0x8d, 0x77, 0x84, 0x0d,
    0x2e, 0x12, 0x9d, 0x4c, 0x5b, 0x47, 0xc0, 0x00, 0x9b, 0xff, 0xee, 0x83,
    0x2c, 0xe1, 0xa6, 0xdf, 0xff, 0xfb, 0x5a, 0x1f, 0xfa, 0xec, 0xff, 0xf5,
    0xd9, 0xff, 0x42, 0xe3, 0x22, 0x00, 0x00, 0x00,
};

TEST(ImageIoTest, LoadWebpLoadsSolisJpgExactly) {
    // photos/people/solis-2c9e.jpg is secretly a WebP file (RIFF/WEBP
    // magic, .jpg extension): wkenc used to fail it with "unknown image
    // type". The pixels below are the exact reference decode (the PPM
    // in libwk-exp-e decodes to these values; lossy WebP decode is
    // deterministic, so exact equality is required, not a tolerance).
    auto loaded = io::load_image_file((photos_root() / "people" / "solis-2c9e.jpg").string());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 663u);
    EXPECT_EQ(loaded->height(), 372u);
    EXPECT_FALSE(loaded->has_alpha());
    EXPECT_EQ(pixel_rgb(*loaded, 0, 0), (std::array<uint8_t, 3>{192, 179, 160}));
    EXPECT_EQ(pixel_rgb(*loaded, 100, 100), (std::array<uint8_t, 3>{3, 3, 3}));
    EXPECT_EQ(pixel_rgb(*loaded, 331, 186), (std::array<uint8_t, 3>{92, 29, 7}));
    EXPECT_EQ(pixel_rgb(*loaded, 662, 371), (std::array<uint8_t, 3>{229, 222, 208}));
    EXPECT_EQ(pixel_rgb(*loaded, 0, 371), (std::array<uint8_t, 3>{151, 133, 114}));
    EXPECT_EQ(pixel_rgb(*loaded, 662, 0), (std::array<uint8_t, 3>{216, 204, 190}));
    EXPECT_EQ(pixel_rgb(*loaded, 200, 50), (std::array<uint8_t, 3>{32, 22, 28}));
}

TEST(ImageIoTest, LoadWebpLoadsPureWebpFile) {
    // Same decoder through a genuine .webp extension; lossless, so
    // every pixel must match exactly.
    auto path = write_temp_bytes("wk_tiny.webp", kTinyLosslessWebp);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 4u);
    EXPECT_EQ(loaded->height(), 4u);
    EXPECT_FALSE(loaded->has_alpha());
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            EXPECT_EQ(pixel_rgb(*loaded, x, y), kTinyLosslessPixels[y * 4 + x])
                << "pixel (" << x << ", " << y << ")";
        }
    }
}

TEST(ImageIoTest, LoadWebpFallsBackForJpegNamedWebp) {
    // The extension can lie the other way too: a JPEG named .webp has
    // no WebP magic, so it falls through to stb (which sniffs content).
    const auto jpg_bytes = read_file_bytes(photos_root() / "people" / "ember-7f3a.jpg");
    auto path = write_temp_bytes("wk_jpeg_named_webp.webp", jpg_bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 225u);
    EXPECT_EQ(loaded->height(), 225u);
    const auto pixel = pixel_rgb(*loaded, 0, 0);
    EXPECT_LE(std::abs(static_cast<int>(pixel[0]) - 42), 3);
    EXPECT_LE(std::abs(static_cast<int>(pixel[1]) - 51), 3);
    EXPECT_LE(std::abs(static_cast<int>(pixel[2]) - 84), 3);
}

TEST(ImageIoTest, LoadWebpRejectsTruncated) {
    const auto solis_bytes = read_file_bytes(photos_root() / "people" / "solis-2c9e.jpg");
    const std::vector<uint8_t> truncated(solis_bytes.begin(), solis_bytes.begin() + 100);
    auto path = write_temp_bytes("wk_truncated.webp", truncated);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadWebpRejectsGarbageWithWebpMagic) {
    // Valid RIFF/WEBP magic, garbage payload: the decoder must reject
    // it cleanly, not crash.
    std::vector<uint8_t> garbage = {0x52, 0x49, 0x46, 0x46, 0x40, 0x00, 0x00, 0x00,
                                    0x57, 0x45, 0x42, 0x50};
    for (int i = 0; i < 64; ++i) {
        garbage.push_back(static_cast<uint8_t>(i * 37 + 11));
    }
    auto path = write_temp_bytes("wk_garbage_magic.webp", garbage);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadWebpRejectsRandomBytesAsWebp) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 64; ++i) {
        bytes.push_back(static_cast<uint8_t>(i * 53 + 7));
    }
    auto path = write_temp_bytes("wk_random.webp", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadWebpRejectsAnimated) {
    // The one-shot decode API cannot handle animation, so a valid
    // animated WebP must be rejected with an error that says so — not a
    // generic decode failure, and not a silent first frame (the old
    // header comment claimed "animated: first frame", which was never
    // true: every animated input was rejected).
    auto path = write_temp_bytes("wk_animated.webp", kAnimatedWebp);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("animated"), std::string::npos)
        << loaded.error().message;
}

// ---- AVIF (vendored libavif + dav1d, decode-only) ----

// Fixtures are stored as hex text (snapshot pushes cannot carry binary
// files); decode pairs of hex digits back to bytes. Mirrors the
// roundtrip_test fixture reader.
std::vector<uint8_t> read_fixture_bytes(const std::string& name) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream in(path);
    if (!in) return {};
    const std::string hex{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (hex.size() % 2 != 0) return {};
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return bytes;
}

std::array<uint16_t, 3> pixel_rgb16(const Image& image, uint32_t x, uint32_t y) {
    const size_t bpp = image.has_alpha() ? 4u : 3u;
    const size_t offset = (static_cast<size_t>(y) * image.width() + x) * bpp * 2;
    const auto pixels = image.pixels();
    const auto sample = [&](size_t o) {
        return static_cast<uint16_t>(pixels[o] | (static_cast<uint16_t>(pixels[o + 1]) << 8));
    };
    return {sample(offset), sample(offset + 2), sample(offset + 4)};
}

// Compare a decoded 10-bit image against ffmpeg's rgb48le reference
// (little-endian uint16 per channel, 10-bit values scaled to 16-bit).
// libavif's YUV->RGB uses its own integer rounding, so the bound is a
// small tolerance in 10-bit units, not exact equality. The exact
// decoder-output check (YUV planes, max diff 0 vs ffmpeg) is done by
// the research harness against the vendored libs, not here.
int max_rgb16_diff_10bit(const Image& image, const std::vector<uint8_t>& expected16) {
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    const uint16_t* expected = reinterpret_cast<const uint16_t*>(expected16.data());
    const uint16_t* got = reinterpret_cast<const uint16_t*>(image.pixels().data());
    int max_diff = 0;
    for (size_t i = 0; i < pixel_count * 3; ++i) {
        // ffmpeg scales 10-bit to 16-bit (x * 65535 / 1023); scale back.
        const int want = (static_cast<int>(expected[i]) * 1023 + 32767) / 65535;
        max_diff = std::max(max_diff, std::abs(static_cast<int>(got[i]) - want));
    }
    return max_diff;
}

void expect_cicp(const Image& image, uint8_t primaries, uint8_t transfer, uint8_t matrix) {
    EXPECT_EQ(image.info().cicp.primaries, primaries);
    EXPECT_EQ(image.info().cicp.transfer, transfer);
    EXPECT_EQ(image.info().cicp.matrix, matrix);
}

TEST(ImageIoTest, LoadAvifTiny444_10bit) {
    // 16x16 gradient, lossless AV1, yuv444p10le, CICP 9/16/9.
    const auto bytes = read_fixture_bytes("tiny_grad_444_10.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_grad_444_10.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 9, 16, 9);
    const auto expected = read_fixture_bytes("tiny_grad_444_10.avif.ffmpeg.rgb.hex");
    ASSERT_EQ(expected.size(), 16u * 16u * 3u * 2u);
    EXPECT_LE(max_rgb16_diff_10bit(*loaded, expected), 4);
}

TEST(ImageIoTest, LoadAvifTiny420_10bit) {
    // 16x16 gradient, lossless AV1, yuv420p10le, CICP 9/16/9.
    // The gradient is smooth on purpose (max step 8 per pixel): with
    // a sharp gradient the 420 chroma upsampler's phase differs
    // between libavif's internal bilinear and ffmpeg's swscale by
    // ~37 LSBs — a real upsampler difference, not a decode error.
    // Even smooth, the two upsamplers differ by a few LSBs, so the
    // bound here is 8, not the 4 used for the 444 fixtures (measured
    // max diff 5 on this fixture).
    const auto bytes = read_fixture_bytes("tiny_grad_420_10.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_grad_420_10.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 9, 16, 9);
    const auto expected = read_fixture_bytes("tiny_grad_420_10.avif.ffmpeg.rgb.hex");
    ASSERT_EQ(expected.size(), 16u * 16u * 3u * 2u);
    EXPECT_LE(max_rgb16_diff_10bit(*loaded, expected), 8);
}

TEST(ImageIoTest, LoadAvifTinySolid444_10bit) {
    // 8x8 solid (512, 256, 768) in 10-bit, lossless AV1, 444, 9/16/9.
    const auto bytes = read_fixture_bytes("tiny_solid_444_10.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_solid_444_10.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 8u);
    EXPECT_EQ(loaded->height(), 8u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    expect_cicp(*loaded, 9, 16, 9);
    const auto expected = read_fixture_bytes("tiny_solid_444_10.avif.ffmpeg.rgb.hex");
    ASSERT_EQ(expected.size(), 8u * 8u * 3u * 2u);
    EXPECT_LE(max_rgb16_diff_10bit(*loaded, expected), 4);
}

TEST(ImageIoTest, LoadAvifTiny444_8bit) {
    // 16x16 gradient, lossless AV1, yuv444p 8-bit, CICP 1/1/1.
    const auto bytes = read_fixture_bytes("tiny_grad_444_8.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_grad_444_8.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits8);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 1, 1, 1);
    const auto expected = read_fixture_bytes("tiny_grad_444_8.avif.ffmpeg.rgb.hex");
    ASSERT_EQ(expected.size(), 16u * 16u * 3u);
    int max_diff = 0;
    const auto pixels = loaded->pixels();
    for (size_t i = 0; i < expected.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(static_cast<int>(pixels[i]) - expected[i]));
    }
    EXPECT_LE(max_diff, 4);
}

// Spot pixels for the research fixtures (512x768, 10-bit, 9/16/9).
// Expected values are ffmpeg rgb48le scaled back to 10-bit
// (x * 1023 / 65535); tolerance +-2 in 10-bit units.
void expect_spot16(const Image& image, uint32_t x, uint32_t y, uint16_t r16, uint16_t g16,
                   uint16_t b16) {
    const auto got = pixel_rgb16(image, x, y);
    const auto want = [&](uint16_t v) { return (static_cast<int>(v) * 1023 + 32767) / 65535; };
    EXPECT_LE(std::abs(static_cast<int>(got[0]) - want(r16)), 2) << "R at (" << x << ", " << y << ")";
    EXPECT_LE(std::abs(static_cast<int>(got[1]) - want(g16)), 2) << "G at (" << x << ", " << y << ")";
    EXPECT_LE(std::abs(static_cast<int>(got[2]) - want(b16)), 2) << "B at (" << x << ", " << y << ")";
}

TEST(ImageIoTest, LoadAvifResearchChart444) {
    const auto bytes = read_fixture_bytes("chart_pq10_444.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_chart_444.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 512u);
    EXPECT_EQ(loaded->height(), 768u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 9, 16, 9);
    expect_spot16(*loaded, 0, 0, 4173, 4173, 4173);
    expect_spot16(*loaded, 100, 100, 9688, 9688, 9688);
    expect_spot16(*loaded, 256, 384, 0, 42778, 42763);
    expect_spot16(*loaded, 511, 767, 24667, 24667, 24667);
}

TEST(ImageIoTest, LoadAvifResearchChart420) {
    const auto bytes = read_fixture_bytes("chart_pq10_420.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_chart_420.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 512u);
    EXPECT_EQ(loaded->height(), 768u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    expect_cicp(*loaded, 9, 16, 9);
    expect_spot16(*loaded, 0, 0, 4173, 4173, 4173);
    expect_spot16(*loaded, 100, 100, 9688, 9688, 9688);
    expect_spot16(*loaded, 256, 384, 0, 42778, 42763);
    expect_spot16(*loaded, 511, 767, 24667, 24667, 24667);
}

TEST(ImageIoTest, LoadAvifResearchChartLossless) {
    // Lossless AV1: decoded pixels must match the source PPM (within
    // conversion rounding) — source chart_pq10.ppm spots in 10-bit.
    const auto bytes = read_fixture_bytes("chart_pq10_444_ll.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_chart_ll.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 512u);
    EXPECT_EQ(loaded->height(), 768u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    expect_cicp(*loaded, 9, 16, 9);
    const auto check = [&](uint32_t x, uint32_t y, uint16_t r, uint16_t g, uint16_t b) {
        const auto got = pixel_rgb16(*loaded, x, y);
        EXPECT_LE(std::abs(static_cast<int>(got[0]) - r), 2) << "R at (" << x << ", " << y << ")";
        EXPECT_LE(std::abs(static_cast<int>(got[1]) - g), 2) << "G at (" << x << ", " << y << ")";
        EXPECT_LE(std::abs(static_cast<int>(got[2]) - b), 2) << "B at (" << x << ", " << y << ")";
    };
    check(0, 0, 64, 64, 64);
    check(100, 100, 151, 151, 151);
    check(256, 384, 0, 668, 668);
    check(511, 767, 385, 385, 385);
}

TEST(ImageIoTest, LoadAvifResearchMemorial444) {
    const auto bytes = read_fixture_bytes("memorial_pq10_444.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_memorial_444.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 512u);
    EXPECT_EQ(loaded->height(), 768u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    expect_cicp(*loaded, 9, 16, 9);
    expect_spot16(*loaded, 0, 0, 8170, 6114, 4502);
    expect_spot16(*loaded, 100, 100, 19613, 18042, 12215);
    expect_spot16(*loaded, 256, 384, 19072, 15503, 10963);
    expect_spot16(*loaded, 511, 767, 14969, 13617, 10497);
}

TEST(ImageIoTest, LoadAvifResearchMemorial420) {
    const auto bytes = read_fixture_bytes("memorial_pq10_420.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_memorial_420.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 512u);
    EXPECT_EQ(loaded->height(), 768u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    expect_cicp(*loaded, 9, 16, 9);
    expect_spot16(*loaded, 0, 0, 7955, 6198, 4490);
    expect_spot16(*loaded, 100, 100, 19464, 17833, 12751);
    expect_spot16(*loaded, 256, 384, 19108, 16286, 11906);
    expect_spot16(*loaded, 511, 767, 14537, 12487, 8991);
}

TEST(ImageIoTest, LoadAvifRejectsAnimated) {
    // Real 2-frame AVIF (brand "avis", ffmpeg/libaom): the one-shot
    // decode API cannot handle animation, reject with an error that
    // says so.
    const auto bytes = read_fixture_bytes("anim_2frame.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_anim.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("animated"), std::string::npos)
        << loaded.error().message;
}

TEST(ImageIoTest, LoadAvifRejectsGrid) {
    // Synthetic ftyp + meta/iinf/infe v2 with item_type "grid": no
    // real grid image is needed — the grid check runs before decode.
    const std::vector<uint8_t> grid = {
        0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x61, 0x76, 0x69, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x61, 0x76, 0x69, 0x66,
        0x00, 0x00, 0x00, 0x2f, 0x6d, 0x65, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x23, 0x69, 0x69, 0x6e, 0x66, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x15, 0x69, 0x6e, 0x66, 0x65, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x67, 0x72, 0x69, 0x64, 0x00};
    const auto decoded = avif::decode(grid);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_NE(decoded.error().message.find("grid"), std::string::npos)
        << decoded.error().message;
    // Control: same boxes with item_type "av01" pass the grid check
    // and fail later in the decoder (not with a grid error).
    std::vector<uint8_t> no_grid = grid;
    no_grid[62] = 0x61;  // 'a'
    no_grid[63] = 0x76;  // 'v'
    no_grid[64] = 0x30;  // '0'
    no_grid[65] = 0x31;  // '1'
    const auto decoded2 = avif::decode(no_grid);
    ASSERT_FALSE(decoded2.has_value());
    EXPECT_EQ(decoded2.error().message.find("grid"), std::string::npos)
        << decoded2.error().message;
}

TEST(ImageIoTest, LoadAvifRejects12Bit) {
    // 12-bit AVIF (ffmpeg/libaom yuv444p12le): out of scope for this
    // slice (8/10-bit only), rejected with an explicit error.
    const auto bytes = read_fixture_bytes("tiny_12bit.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_12bit.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("bit depth"), std::string::npos)
        << loaded.error().message;
}

// Rewrite the ispe width/height of an AVIF byte string in place
// (ispe box: size + type + version/flags + width + height, all big
// endian). Returns nullopt when no ispe box is found.
std::optional<std::vector<uint8_t>> patch_ispe_dims(const std::vector<uint8_t>& bytes,
                                                    uint32_t width, uint32_t height) {
    static constexpr uint8_t kIspe[4] = {'i', 's', 'p', 'e'};
    for (size_t i = 0; i + 16 <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, kIspe, 4) != 0) continue;
        std::vector<uint8_t> patched = bytes;
        const auto put_u32be = [&](size_t at, uint32_t v) {
            patched[at] = static_cast<uint8_t>(v >> 24);
            patched[at + 1] = static_cast<uint8_t>(v >> 16);
            patched[at + 2] = static_cast<uint8_t>(v >> 8);
            patched[at + 3] = static_cast<uint8_t>(v);
        };
        put_u32be(i + 8, width);
        put_u32be(i + 12, height);
        return patched;
    }
    return std::nullopt;
}

TEST(ImageIoTest, LoadAvifRejectsIspeDimsLie) {
    // ispe says 2048x2048 but the AV1 frame is 16x16: the decoder must
    // reject the file instead of scaling the frame up to the ispe size
    // (fuzz finding 1: a 16384x16384 lie cost 33 s / 4.6 GB).
    const auto bytes = read_fixture_bytes("tiny_grad_444_10.avif.hex");
    ASSERT_FALSE(bytes.empty());
    const auto lied = patch_ispe_dims(bytes, 2048, 2048);
    ASSERT_TRUE(lied.has_value());
    const auto decoded = avif::decode(*lied);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_NE(decoded.error().message.find("ispe"), std::string::npos)
        << decoded.error().message;
    // Control: the same patcher writing the true 16x16 dims still
    // decodes — the check must not reject a truthful ispe.
    const auto honest = patch_ispe_dims(bytes, 16, 16);
    ASSERT_TRUE(honest.has_value());
    const auto decoded2 = avif::decode(*honest);
    ASSERT_TRUE(decoded2.has_value()) << decoded2.error().message;
    EXPECT_EQ(decoded2->width(), 16u);
    EXPECT_EQ(decoded2->height(), 16u);
}

TEST(ImageIoTest, LoadAvifRejectsIspeDimsLieHuge) {
    // The exact fuzz reproducer: ispe 16384x16384 on a 16x16 frame.
    // Pre-fix this allocates ~4.6 GB; post-fix it must fail fast.
    const auto bytes = read_fixture_bytes("tiny_grad_444_10.avif.hex");
    ASSERT_FALSE(bytes.empty());
    const auto lied = patch_ispe_dims(bytes, 16384, 16384);
    ASSERT_TRUE(lied.has_value());
    const auto decoded = avif::decode(*lied);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_NE(decoded.error().message.find("ispe"), std::string::npos)
        << decoded.error().message;
}

TEST(ImageIoTest, LoadAvifRejectsFrameSizeOverride) {
    // The sequence header max dims agree with the ispe, but the frame
    // header overrides the frame size down to 16x16: the decoder must
    // reject instead of scaling the frame up to the ispe size (the
    // 16384 variant costs 183 s / 4.6 GB pre-fix, same as the plain
    // ispe lie). Fixtures are the proven attack files from the
    // frame_size_override diagnosis (672 bytes each).
    for (const char* name :
         {"fso_2048.avif.hex", "fso_4096.avif.hex", "fso_8192.avif.hex", "fso_16384.avif.hex"}) {
        const auto bytes = read_fixture_bytes(name);
        ASSERT_FALSE(bytes.empty()) << name;
        const auto decoded = avif::decode(bytes);
        ASSERT_FALSE(decoded.has_value()) << name;
        EXPECT_NE(decoded.error().message.find("ispe"), std::string::npos)
            << name << ": " << decoded.error().message;
    }
}

TEST(ImageIoTest, LoadAvifRejectsFrameSizeOverride8Bit) {
    // Same override on an 8-bit seed: the mechanism does not depend
    // on bit depth, but prove it with numbers (513-byte file crafted
    // from avif_tiny_444_8.avif the same way as the 10-bit variants).
    const auto bytes = read_fixture_bytes("fso_2048_8bit.avif.hex");
    ASSERT_FALSE(bytes.empty());
    const auto decoded = avif::decode(bytes);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_NE(decoded.error().message.find("ispe"), std::string::npos)
        << decoded.error().message;
}

TEST(ImageIoTest, LoadAvifRejectsTruncated) {
    const auto bytes = read_fixture_bytes("chart_pq10_444.avif.hex");
    ASSERT_FALSE(bytes.empty());
    const std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 100);
    auto path = write_temp_bytes("wk_truncated.avif", truncated);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadAvifRejectsGarbageWithAvifMagic) {
    // Valid ftyp/avif magic, garbage payload: reject cleanly, no crash.
    std::vector<uint8_t> garbage = {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70,
                                    0x61, 0x76, 0x69, 0x66, 0x00, 0x00, 0x00, 0x00,
                                    0x61, 0x76, 0x69, 0x66};
    for (int i = 0; i < 64; ++i) {
        garbage.push_back(static_cast<uint8_t>(i * 37 + 11));
    }
    auto path = write_temp_bytes("wk_garbage_magic.avif", garbage);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadAvifRejectsRandomBytes) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 64; ++i) {
        bytes.push_back(static_cast<uint8_t>(i * 53 + 7));
    }
    auto path = write_temp_bytes("wk_random.avif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadAvifViaMagicSniff) {
    // AVIF bytes wearing a .jpg extension: magic sniff routes to the
    // AVIF decoder, like the WebP solis case.
    const auto bytes = read_fixture_bytes("chart_pq10_444.avif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_avif_named_jpg.jpg", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 512u);
    EXPECT_EQ(loaded->height(), 768u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
}

TEST(ImageIoTest, LoadAvifFallsBackForJpegNamedAvif) {
    // A JPEG named .avif has no AVIF magic, so it falls through to stb
    // (which sniffs content) — mirrors the WebP fallback test.
    const auto jpg_bytes = read_file_bytes(photos_root() / "people" / "ember-7f3a.jpg");
    auto path = write_temp_bytes("wk_jpeg_named_avif.avif", jpg_bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 225u);
    EXPECT_EQ(loaded->height(), 225u);
}

// ---- JXL (vendored libjxl 0.13.0, decode-only, native only) ----

// Fixtures are the tiny JXL files built for the slice (see
// tools/jxl_slice_2026-11-20.md): 16x16/8x8, lossless JXL, so the
// decoded pixels must match the ffmpeg reference EXACTLY (max diff
// 0) — unlike AVIF, there is no YUV->RGB rounding here: JXL decodes
// straight to RGB, and the 10-bit values libjxl scales to 16-bit
// (x * 65535 / 1023) are scaled back exactly by the decoder
// (round(x * 1023 / 65535) recovers every 10-bit value: the forward
// error is at most 1 LSB in 16-bit units, i.e. 0.016 in 10-bit
// units). The research harness proved 0 diff on all five fixtures
// against ffmpeg 8.1.2 (libjxl 0.7) and against libjxl 0.13 itself.
TEST(ImageIoTest, LoadJxlTinyGrad10bit) {
    // 16x16 gradient, lossless JXL, 10-bit, CICP 9/16/9.
    const auto bytes = read_fixture_bytes("tiny_grad_10.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_grad_10.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 9, 16, 9);
    const auto expected = read_fixture_bytes("tiny_grad_10.jxl.ffmpeg.rgb16.hex");
    ASSERT_EQ(expected.size(), 16u * 16u * 3u * 2u);
    EXPECT_EQ(max_rgb16_diff_10bit(*loaded, expected), 0);
}

TEST(ImageIoTest, LoadJxlTinyGrad8bit) {
    // 16x16 gradient, lossless JXL, 8-bit, CICP 1/13/1.
    const auto bytes = read_fixture_bytes("tiny_grad_8.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_grad_8.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits8);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 1, 13, 1);
    const auto expected = read_fixture_bytes("tiny_grad_8.jxl.ffmpeg.rgb.hex");
    ASSERT_EQ(expected.size(), 16u * 16u * 3u);
    const auto pixels = loaded->pixels();
    int max_diff = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(static_cast<int>(pixels[i]) - expected[i]));
    }
    EXPECT_EQ(max_diff, 0);
}

TEST(ImageIoTest, LoadJxlTinySolid10bit) {
    // 8x8 solid (512, 256, 768) in 10-bit, lossless JXL, 9/16/9.
    const auto bytes = read_fixture_bytes("tiny_solid_10.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_solid_10.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 8u);
    EXPECT_EQ(loaded->height(), 8u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    expect_cicp(*loaded, 9, 16, 9);
    const auto expected = read_fixture_bytes("tiny_solid_10.jxl.ffmpeg.rgb16.hex");
    ASSERT_EQ(expected.size(), 8u * 8u * 3u * 2u);
    EXPECT_EQ(max_rgb16_diff_10bit(*loaded, expected), 0);
}

TEST(ImageIoTest, LoadJxlTinyGray) {
    // 8x8 gray, lossless JXL, 8-bit. libjxl is asked for 3 channels,
    // so it replicates gray to RGB itself; the carried primaries are
    // 0 (gray), which passes the H.273 check as-is.
    const auto bytes = read_fixture_bytes("tiny_gray.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_gray.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 8u);
    EXPECT_EQ(loaded->height(), 8u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits8);
    EXPECT_FALSE(loaded->has_alpha());
    expect_cicp(*loaded, 0, 13, 1);
    const auto expected = read_fixture_bytes("tiny_gray.jxl.ffmpeg.rgb.hex");
    ASSERT_EQ(expected.size(), 8u * 8u * 3u);
    const auto pixels = loaded->pixels();
    int max_diff = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(static_cast<int>(pixels[i]) - expected[i]));
    }
    EXPECT_EQ(max_diff, 0);
}

TEST(ImageIoTest, LoadJxlTinyAlpha) {
    // 8x8 RGBA (PNG source), lossless JXL, 8-bit, CICP 1/13/1.
    const auto bytes = read_fixture_bytes("tiny_alpha.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_tiny_alpha.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 8u);
    EXPECT_EQ(loaded->height(), 8u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits8);
    EXPECT_TRUE(loaded->has_alpha());
    expect_cicp(*loaded, 1, 13, 1);
    const auto expected = read_fixture_bytes("tiny_alpha.jxl.ffmpeg.rgba.hex");
    ASSERT_EQ(expected.size(), 8u * 8u * 4u);
    const auto pixels = loaded->pixels();
    int max_diff = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(static_cast<int>(pixels[i]) - expected[i]));
    }
    EXPECT_EQ(max_diff, 0);
}

TEST(ImageIoTest, LoadJxlHasMagic) {
    // Raw codestream (0xFF 0x0A) and the 12-byte signature box
    // ("JXL ") are both JXL; anything else is not. The box type is
    // uppercase "JXL ", not the lowercase "jxl " of the ftyp brand
    // or the jxlc box — a hand-rolled check got this wrong (the
    // 16-bit container fixture), so the decoder uses libjxl's own
    // JxlSignatureCheck.
    EXPECT_TRUE(jxl::has_jxl_magic(std::vector<uint8_t>{0xFF, 0x0A, 0x43, 0x04}));
    const std::vector<uint8_t> box = {0x00, 0x00, 0x00, 0x0C, 0x4A, 0x58,
                                      0x4C, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
    EXPECT_TRUE(jxl::has_jxl_magic(box));
    EXPECT_FALSE(jxl::has_jxl_magic(std::vector<uint8_t>{0x00, 0x00, 0x00, 0x0C,
                                                         0x6A, 0x78, 0x6C, 0x20}));
    EXPECT_FALSE(jxl::has_jxl_magic(std::vector<uint8_t>{0x89, 0x50, 0x4E, 0x47}));
    EXPECT_FALSE(jxl::has_jxl_magic(std::vector<uint8_t>{0xFF}));
}

TEST(ImageIoTest, LoadJxlRejectsAnimated) {
    // Real 2-frame JXL: the one-shot decode API cannot handle
    // animation, reject with an error that says so.
    const auto bytes = read_fixture_bytes("tiny_anim.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_anim.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("animated"), std::string::npos)
        << loaded.error().message;
}

TEST(ImageIoTest, LoadGifRejectsAnimated) {
    // Real 2-frame GIF (16x16, red then blue): the one-shot decode
    // API cannot handle animation, reject with an error that says so
    // — not a silent first frame, like WebP/AVIF/JXL.
    const auto bytes = read_fixture_bytes("anim_2frame.gif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_anim.gif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("animated"), std::string::npos)
        << loaded.error().message;
}

TEST(ImageIoTest, LoadGifAcceptsSingleFrame) {
    // A 1-frame GIF still loads like any other still image.
    const auto bytes = read_fixture_bytes("single_frame.gif.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_single.gif", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits8);
}

TEST(ImageIoTest, LoadJxlRejects12Bit) {
    // 12-bit JXL: out of scope for this slice (8/10-bit only),
    // rejected with an explicit error.
    const auto bytes = read_fixture_bytes("tiny_grad_12.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_12bit.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("bit depth"), std::string::npos)
        << loaded.error().message;
}

TEST(ImageIoTest, LoadJxlRejects16Bit) {
    // 16-bit JXL (box container form): same rejection as 12-bit.
    const auto bytes = read_fixture_bytes("tiny_grad_16.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_16bit.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("bit depth"), std::string::npos)
        << loaded.error().message;
}

TEST(ImageIoTest, LoadJxlRejectsLinearTransfer) {
    // Custom-gamma ICC (2.2): libjxl 0.13 reports the file's transfer
    // as LINEAR (8), and there is no H.273 code for linear — the
    // gamma was lost at encode time (libjxl's ICC->fields
    // conversion), not at decode. Reject, do not relabel.
    const auto bytes = read_fixture_bytes("tiny_custom_gamma.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_gamma.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().message.find("transfer"), std::string::npos)
        << loaded.error().message;
}

TEST(ImageIoTest, LoadJxlRejectsTruncated) {
    const auto bytes = read_fixture_bytes("tiny_grad_10.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    const std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 100);
    auto path = write_temp_bytes("wk_truncated.jxl", truncated);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadJxlRejectsGarbageWithJxlMagic) {
    // Valid JXL codestream magic, garbage payload: reject cleanly,
    // no crash.
    std::vector<uint8_t> garbage = {0xFF, 0x0A};
    for (int i = 0; i < 64; ++i) {
        garbage.push_back(static_cast<uint8_t>(i * 37 + 11));
    }
    auto path = write_temp_bytes("wk_garbage_magic.jxl", garbage);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadJxlRejectsRandomBytes) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 64; ++i) {
        bytes.push_back(static_cast<uint8_t>(i * 53 + 7));
    }
    auto path = write_temp_bytes("wk_random.jxl", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}

TEST(ImageIoTest, LoadJxlViaMagicSniff) {
    // JXL bytes wearing a .jpg extension: magic sniff routes to the
    // JXL decoder, like the WebP/AVIF cases.
    const auto bytes = read_fixture_bytes("tiny_grad_10.jxl.hex");
    ASSERT_FALSE(bytes.empty());
    auto path = write_temp_bytes("wk_jxl_named_jpg.jpg", bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 16u);
    EXPECT_EQ(loaded->height(), 16u);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
}

TEST(ImageIoTest, LoadJxlFallsBackForJpegNamedJxl) {
    // A JPEG named .jxl has no JXL magic, so it falls through to stb
    // (which sniffs content) — mirrors the WebP/AVIF fallback tests.
    const auto jpg_bytes = read_file_bytes(photos_root() / "people" / "ember-7f3a.jpg");
    auto path = write_temp_bytes("wk_jpeg_named_jxl.jxl", jpg_bytes);
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_EQ(loaded->width(), 225u);
    EXPECT_EQ(loaded->height(), 225u);
}
