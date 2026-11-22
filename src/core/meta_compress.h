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

#include "common.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wk::meta
{

// Largest output a decompressed META payload may expand to. Real META
// payloads are kilobytes (largest measured: 1,585 B); the cap only
// bounds a decompression bomb, it never triggers on legitimate metadata.
inline constexpr size_t kMaxDecompressedMetaPayload = 16 * 1024 * 1024;

// Compress a serialized META payload with zlib (level 6). Returns the
// compressed bytes only when strictly smaller than the input;
// otherwise returns nullopt and the caller keeps the raw payload.
// The flag bit that signals compression costs no bytes of its own, so
// any strict saving is a net win; a payload that does not shrink
// (the 15 B quality-score baseline grows to 23 B) stays raw. #meta
[[nodiscard]] std::optional<std::vector<uint8_t>>
compress_meta_payload(std::span<const uint8_t> raw);

// Decompress a zlib-compressed META payload. Fails when the input is
// not a valid zlib stream, has trailing bytes after the stream, or
// would expand past kMaxDecompressedMetaPayload. #meta
[[nodiscard]] Result<std::vector<uint8_t>>
decompress_meta_payload(std::span<const uint8_t> compressed);

} // namespace wk::meta
