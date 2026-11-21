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

namespace wk::avif {

// ftyp box with major brand "avif" (still) or "avis" (sequence):
// 4 bytes size, "ftyp" at 4, brand at 8.
bool has_avif_magic(std::span<const uint8_t> bytes);

// Decode a still AVIF (8/10-bit, 444/420) into an Image. The Image
// carries the file's embedded CICP, normalized to what the encoder
// supports (see avif_decode.cpp). Animated and grid AVIFs are
// rejected with explicit errors, as are files whose ispe dimensions
// disagree with the AV1 frame dimensions from the sequence header.
// Fails cleanly on malformed input (no crash/UB).
Result<Image> decode(std::span<const uint8_t> bytes);

}  // namespace wk::avif
