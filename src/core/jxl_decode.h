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
// distributed under the Apache License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <wk/wk.hpp>
#include <cstdint>
#include <span>

namespace wk::jxl {

// JXL signature: raw codestream (0xFF 0x0A) or the 12-byte signature
// box (size 12, "JXL ", 0D 0A 87 0A). Uses libjxl's own
// JxlSignatureCheck instead of a hand-rolled check: the box type is
// uppercase "JXL ", not the lowercase "jxl " of the ftyp brand or
// the jxlc box, and a hand-rolled check got that wrong.
bool has_jxl_magic(std::span<const uint8_t> bytes);

// Decode a still JXL (8/10-bit, RGB or gray or RGBA) into an Image.
// The Image carries the file's embedded CICP, normalized to what
// the encoder supports (see jxl_decode.cpp). Animated JXLs and
// 12/16-bit JXLs are rejected with explicit errors. Fails cleanly
// on malformed input (no crash/UB). Native only: JXL is not part of
// the web build.
Result<Image> decode(std::span<const uint8_t> bytes);

}  // namespace wk::jxl
