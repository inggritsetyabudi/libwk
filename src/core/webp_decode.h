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
#pragma once

#include <wk/wk.hpp>
#include <cstdint>
#include <span>

namespace wk::webp {

// RIFF....WEBP magic: 12 bytes, "RIFF" at 0, "WEBP" at 8.
bool has_webp_magic(std::span<const uint8_t> bytes);

// Decode an 8-bit still WebP (lossy or lossless) into an 8-bit Image.
// Animated WebP is not supported and is rejected. Fails cleanly on
// malformed input (no crash/UB).
Result<Image> decode(std::span<const uint8_t> bytes);

}  // namespace wk::webp
