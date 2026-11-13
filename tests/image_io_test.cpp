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
#include "../src/image_io.h"

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

TEST(ImageIoTest, LoadPpmRejectsTruncatedPayload) {
    // Control: the pre-existing payload-size check still works.
    auto path = write_temp_ppm("wk_short.ppm", "P6\n2 2\n255\n",
                               std::vector<uint8_t>(6, 0));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_FALSE(loaded.has_value());
}
