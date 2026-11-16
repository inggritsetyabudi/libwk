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

#include "cclm.h"
#pragma once

#include "common.h"
#include "tile_util.h"
#include <cstdint>
#include <span>

namespace wk {

Result<TileDecodeResult> decode_lossy_tile(std::span<const uint8_t> tile_data,
                                            uint16_t tile_x, uint16_t tile_y,
                                            uint32_t tile_size,
                                            uint32_t image_width, uint32_t image_height,
                                            uint8_t bit_depth,
                                            ChromaSubsampling subsampling,
                                            uint8_t layer_flags,
                                            bool has_alpha,
                                            bool tile_has_alpha);

}  // namespace wk
