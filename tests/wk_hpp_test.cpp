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

// This file deliberately includes ONLY the public header: if wk.hpp is
// not self-contained, this translation unit fails to compile. Before the
// PR5 fix it failed with "invalid use of incomplete type 'wk::Error'"
// because Error was only forward-declared here and fully defined in the
// internal src/common.h.
#include <wk/wk.hpp>

#include <gtest/gtest.h>

TEST(WkHppTest, PublicHeaderIsSelfContained) {
    wk::Image image(16, 16, wk::BitDepth::Bits8, false);
    wk::EncoderConfig config;
    config.quality = 75.0f;

    auto encoded = wk::encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    auto info = wk::get_info(*encoded);
    ASSERT_TRUE(info.has_value()) << info.error().message;
    EXPECT_EQ(info->width, 16u);
    EXPECT_EQ(info->height, 16u);

    auto decoded = wk::decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width(), 16u);
    EXPECT_EQ(decoded->height(), 16u);

    // The error type itself must be usable: code + message are part of
    // the public API surface.
    wk::Error error{wk::ErrorCode::EncodeFailed, "test"};
    EXPECT_EQ(error.code, wk::ErrorCode::EncodeFailed);
    EXPECT_EQ(error.message, "test");
}
