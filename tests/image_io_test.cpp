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
#include <filesystem>
#include <fstream>
#include "../src/core/image_io.h"

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
