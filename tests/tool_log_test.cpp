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

// Unit tests for the CLI colored logger (tools/tool_log.h): the
// PSNR/SSIM threshold colors and the plain-vs-colored switch. The
// level filtering and TTY auto-detection are exercised by the tools
// themselves (wkenc/wkdec/wkmetric) and are not unit-tested here.

#include <gtest/gtest.h>
#include "../tools/tool_log.h"

TEST(ToolLogTest, ColorizePlainWhenColorOff) {
    EXPECT_EQ(wk::tool::colorize("44.77", "\033[1;32m", false), "44.77");
    EXPECT_EQ(wk::tool::color_psnr(44.77, false), "44.77");
    EXPECT_EQ(wk::tool::color_ssim(0.9914, false), "0.9914");
}

TEST(ToolLogTest, ColorizeWrapsWhenColorOn) {
    EXPECT_EQ(wk::tool::colorize("44.77", "\033[1;32m", true), "\033[1;32m44.77\033[0m");
}

TEST(ToolLogTest, PsnrThresholds) {
    // Green >= 35, yellow 30..35, red < 30.
    EXPECT_EQ(wk::tool::color_psnr(44.77, true), "\033[1;32m44.77\033[0m");
    EXPECT_EQ(wk::tool::color_psnr(35.00, true), "\033[1;32m35.00\033[0m");
    EXPECT_EQ(wk::tool::color_psnr(32.50, true), "\033[33m32.50\033[0m");
    EXPECT_EQ(wk::tool::color_psnr(30.00, true), "\033[33m30.00\033[0m");
    EXPECT_EQ(wk::tool::color_psnr(28.10, true), "\033[1;31m28.10\033[0m");
}

TEST(ToolLogTest, SsimThresholds) {
    // Green >= 0.95, yellow 0.90..0.95, red < 0.90.
    EXPECT_EQ(wk::tool::color_ssim(0.9914, true), "\033[1;32m0.9914\033[0m");
    EXPECT_EQ(wk::tool::color_ssim(0.9500, true), "\033[1;32m0.9500\033[0m");
    EXPECT_EQ(wk::tool::color_ssim(0.9200, true), "\033[33m0.9200\033[0m");
    EXPECT_EQ(wk::tool::color_ssim(0.9000, true), "\033[33m0.9000\033[0m");
    EXPECT_EQ(wk::tool::color_ssim(0.8500, true), "\033[1;31m0.8500\033[0m");
}
