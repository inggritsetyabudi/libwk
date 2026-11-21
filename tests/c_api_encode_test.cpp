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

// C API encode tests on full-entropy pixels. These are the slow half
// of c_api_test, split out so the fast smoke test stays fast: under
// ASan Debug one 16x16 full-entropy encode costs ~65 s (the cluster
// optimizer tries every coefficient context), so the six combos here
// take ~22-35 min. See tools/holes_asan_stall_2026-11-21.md.
// The pixel pattern is deliberately NOT weakened: it must stay the
// same full-entropy noise the encoder is validated against.
// #asan-gates

#include <gtest/gtest.h>
#include <cstdint>
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

TEST(CApiEncodeTest, EncodeAcceptsValidCombos) {
    // Control: every consistent (bit_depth, bpp) pair encodes fine.
    // Moved verbatim from c_api_test: full-entropy pixels, all 6 combos.
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

TEST(CApiEncodeTest, EncodeAcceptsPaddedStride) {
    // Control: an explicit stride larger than the row size is fine; the
    // padding bytes between rows are ignored. Moved verbatim from
    // c_api_test: the sawtooth fill is just as slow under ASan as the
    // noise pattern (~51 s measured), so it belongs in this gate.
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
