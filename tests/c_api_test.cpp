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
#include <cstdint>
#include <cstring>
#include <vector>
#include <wk/wk.h>

namespace {

wk_encoder_config_t make_config(uint8_t bit_depth) {
    wk_encoder_config_t config;
    wk_encoder_config_init(&config);
    config.bit_depth = bit_depth;
    return config;
}

std::vector<uint8_t> make_pixels(uint32_t width, uint32_t height, uint8_t bpp) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * bpp);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>((i * 37) & 0xFF);
    }
    return pixels;
}

wk_error_t try_encode(uint32_t width, uint32_t height, uint32_t stride, uint8_t bpp,
                      const wk_encoder_config_t& config, uint8_t** out_data, size_t* out_size) {
    auto pixels = make_pixels(width, height, bpp);
    return wk_encode(pixels.data(), width, height, stride, bpp, &config, out_data, out_size);
}

}  // namespace

TEST(CApiTest, EncodeRejectsNullParams) {
    wk_encoder_config_t config = make_config(8);
    auto pixels = make_pixels(16, 16, 3);
    uint8_t* out_data = nullptr;
    size_t out_size = 0;

    EXPECT_EQ(wk_encode(nullptr, 16, 16, 0, 3, &config, &out_data, &out_size),
              WK_ERROR_INVALID_PARAM);
    EXPECT_EQ(wk_encode(pixels.data(), 16, 16, 0, 3, nullptr, &out_data, &out_size),
              WK_ERROR_INVALID_PARAM);
    EXPECT_EQ(wk_encode(pixels.data(), 16, 16, 0, 3, &config, nullptr, &out_size),
              WK_ERROR_INVALID_PARAM);
    EXPECT_EQ(wk_encode(pixels.data(), 16, 16, 0, 3, &config, &out_data, nullptr),
              WK_ERROR_INVALID_PARAM);
}

TEST(CApiTest, EncodeRejectsZeroDimensions) {
    // Use a real, non-empty caller buffer: with width 0 the encoder must
    // still reject the call instead of relying on an empty vector's
    // data() happening to be null.
    wk_encoder_config_t config = make_config(8);
    std::vector<uint8_t> pixels(48, 0xAB);
    uint8_t* out_data = nullptr;
    size_t out_size = 0;

    EXPECT_EQ(wk_encode(pixels.data(), 0, 16, 0, 3, &config, &out_data, &out_size),
              WK_ERROR_INVALID_PARAM);
    EXPECT_EQ(wk_encode(pixels.data(), 16, 0, 0, 3, &config, &out_data, &out_size),
              WK_ERROR_INVALID_PARAM);
}

TEST(CApiTest, EncodeRejectsInvalidBitDepth) {
    // Any bit depth other than 8/10/12 used to be silently treated as 8-bit.
    uint8_t* out_data = nullptr;
    size_t out_size = 0;

    for (uint8_t bad_depth : {0, 7, 9, 11, 16, 32}) {
        wk_encoder_config_t config = make_config(bad_depth);
        EXPECT_EQ(try_encode(16, 16, 0, 3, config, &out_data, &out_size),
                  WK_ERROR_INVALID_PARAM)
            << "bit_depth " << static_cast<int>(bad_depth) << " was accepted";
    }
}

TEST(CApiTest, EncodeRejectsMismatchedBpp) {
    // bpp must agree with bit depth: 3/4 at 8-bit, 6/8 at 10/12-bit.
    // A mismatched bpp used to make wk_encode memcpy past the Image rows:
    // e.g. bpp 6 at 8-bit copied 2x the row size (heap overflow).
    uint8_t* out_data = nullptr;
    size_t out_size = 0;

    const std::vector<std::pair<uint8_t, uint8_t>> bad_cases = {
        {8, 6}, {8, 8}, {8, 5}, {8, 2}, {8, 1}, {8, 0}, {8, 7},
        {10, 3}, {10, 4}, {10, 5}, {10, 7},
        {12, 3}, {12, 4},
    };
    for (const auto& [depth, bpp] : bad_cases) {
        wk_encoder_config_t config = make_config(depth);
        EXPECT_EQ(try_encode(16, 16, 0, bpp, config, &out_data, &out_size),
                  WK_ERROR_INVALID_PARAM)
            << "bit_depth " << static_cast<int>(depth) << " with bpp " << static_cast<int>(bpp)
            << " was accepted";
    }
}

TEST(CApiTest, EncodeRejectsShortStride) {
    // An explicit stride smaller than the row size would make the row
    // copies overlap and read past the caller's rows.
    wk_encoder_config_t config = make_config(8);
    uint8_t* out_data = nullptr;
    size_t out_size = 0;

    EXPECT_EQ(try_encode(16, 16, 32, 3, config, &out_data, &out_size),
              WK_ERROR_INVALID_PARAM);
    EXPECT_EQ(try_encode(16, 16, 1, 3, config, &out_data, &out_size),
              WK_ERROR_INVALID_PARAM);
}

TEST(CApiTest, EncodeAcceptsValidCombos) {
    // Control: every consistent (bit_depth, bpp) pair encodes fine.
    const std::vector<std::pair<uint8_t, uint8_t>> good_cases = {
        {8, 3}, {8, 4}, {10, 6}, {10, 8}, {12, 6}, {12, 8},
    };
    for (const auto& [depth, bpp] : good_cases) {
        wk_encoder_config_t config = make_config(depth);
        uint8_t* out_data = nullptr;
        size_t out_size = 0;
        ASSERT_EQ(try_encode(16, 16, 0, bpp, config, &out_data, &out_size), WK_OK)
            << "bit_depth " << static_cast<int>(depth) << " bpp " << static_cast<int>(bpp);
        ASSERT_NE(out_data, nullptr);
        ASSERT_GT(out_size, 0u);

        wk_image_info_t info{};
        ASSERT_EQ(wk_get_info(out_data, out_size, &info), WK_OK);
        EXPECT_EQ(info.width, 16u);
        EXPECT_EQ(info.height, 16u);
        EXPECT_EQ(info.bit_depth, depth);
        wk_free(out_data);
    }
}

TEST(CApiTest, EncodeAcceptsPaddedStride) {
    // Control: an explicit stride larger than the row size is fine; the
    // padding bytes between rows are ignored.
    wk_encoder_config_t config = make_config(8);
    const uint32_t width = 16, height = 16, bpp = 3;
    const uint32_t row_bytes = width * bpp;
    const uint32_t stride = row_bytes + 16;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height, 0xAB);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < row_bytes; ++x) {
            pixels[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>((x * 13 + y) & 0xFF);
        }
    }

    uint8_t* out_data = nullptr;
    size_t out_size = 0;
    ASSERT_EQ(wk_encode(pixels.data(), width, height, stride, bpp, &config, &out_data, &out_size),
              WK_OK);
    ASSERT_NE(out_data, nullptr);
    wk_image_info_t info{};
    ASSERT_EQ(wk_get_info(out_data, out_size, &info), WK_OK);
    EXPECT_EQ(info.width, width);
    EXPECT_EQ(info.height, height);
    wk_free(out_data);
}
