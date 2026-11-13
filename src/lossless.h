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
#include "rans.h"
#include <array>
#include <vector>

namespace wk {



enum class LosslessPred : uint8_t {
    NONE     = 0,
    L        = 1,
    T        = 2,
    TR       = 3,
    TL       = 4,
    AVG_LT   = 5,
    AVG_LTR  = 6,
    AVG_LTL  = 7,
    AVG_TTR  = 8,
    AVG_TTL  = 9,
    AVG_LTRTL= 10,
    SELECT   = 11,
    CLAMP_ADD= 12,
    NUM_PREDS= 13,
};




void apply_subtract_green(uint8_t* rgba, uint32_t width, uint32_t height);
void undo_subtract_green(uint8_t* rgba, uint32_t width, uint32_t height);



struct Palette {
    uint32_t colors[256];
    int      count = 0;
};


bool build_palette(const uint8_t* rgba, uint32_t width, uint32_t height,
                   Palette& palette);


void apply_palette(const uint8_t* rgba, uint32_t width, uint32_t height,
                   const Palette& palette, uint8_t* indices);


void undo_palette(const uint8_t* indices, uint32_t width, uint32_t height,
                  const Palette& palette, uint8_t* rgba);



struct LzToken {
    enum Type : uint8_t { LITERAL, BACKREFERENCE };
    Type     type;
    uint8_t  literal;
    uint16_t distance;
    uint16_t length;
};

std::vector<LzToken> lz_encode_indices(const uint8_t* indices, size_t count);
// Decodes LZ tokens into exactly `count` indices. Every bound is checked
// against the caller-supplied output size because the token stream comes
// from an untrusted file: unknown types, distances that reach before the
// start of the output, and streams that produce too few or too many
// indices are all rejected instead of reading/writing out of bounds.
Result<void> lz_decode_indices(const std::vector<LzToken>& tokens, uint8_t* indices, size_t count);



struct EntropyImage {
    uint8_t  subsample_log2 = 2;
    uint32_t meta_width  = 0;
    uint32_t meta_height = 0;
    std::vector<uint8_t> context_map;
    int num_context_sets = 1;
};


EntropyImage build_entropy_image(const uint8_t* rgba, uint32_t width, uint32_t height,
                                  int max_context_sets = 12);


inline int get_context_set(const EntropyImage& ei, uint32_t x, uint32_t y) {
    uint32_t mx = x >> ei.subsample_log2;
    uint32_t my = y >> ei.subsample_log2;
    mx = std::min(mx, ei.meta_width - 1);
    my = std::min(my, ei.meta_height - 1);
    return ei.context_map[my * ei.meta_width + mx];
}




[[nodiscard]] Result<std::vector<uint8_t>> lossless_encode(
    const uint8_t* rgba, uint32_t width, uint32_t height,
    uint8_t bit_depth);


[[nodiscard]] Result<std::vector<uint8_t>> lossless_decode(
    std::span<const uint8_t> data, uint32_t width, uint32_t height,
    uint8_t bit_depth);

}
