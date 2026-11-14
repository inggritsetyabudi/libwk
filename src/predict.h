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
#include "dct.h"
#include "lossy_coeff_stream.h"
#include "quantize.h"
#include <array>
#include <cstdint>

namespace wk {



enum class PredMode : uint8_t {
    DC       = 0,
    V        = 1,
    H        = 2,
    TM       = 3,
    DC_LEFT  = 4,
    DC_TOP   = 5,
    DC_128   = 6,
    D45      = 7,
    D135     = 8,
    D117     = 9,
    D153     = 10,
    D207     = 11,
    D63      = 12,
    PLANAR   = 13,
    // Cross-component linear model (chroma only): predict chroma from
    // the co-located reconstructed luma as a*luma + b, with (a, b)
    // derived from the neighbouring reconstructed luma/chroma
    // (see cclm.h). Only valid for chroma blocks in layout V10+; a
    // decoder MUST reject CCLM in a luma mode stream or in a layout
    // below V10. The mode stream alphabet grows to 15 (0-14).
    CCLM     = 14,
    NUM_MODES = 15,
};











void predict_8x8(PredMode mode, const int16_t* above, const int16_t* left,
                  int16_t above_left, int16_t* pred, int16_t max_val);



struct RdResult {
    PredMode mode;
    float    cost;
};










RdResult select_best_mode(const int16_t* original, const int16_t* above,
                           const int16_t* left, int16_t above_left,
                           float lambda, int16_t max_val,
                           const QuantTable& quant, uint8_t bit_depth,
                           float quant_scale = 1.0f);

PredMode select_deterministic_mode(const int16_t* above, const int16_t* left,
                                   int16_t above_left, int16_t max_val);

float compute_ssd_8x8(const int16_t* a, const int16_t* b);


float estimate_bits_8x8(const int16_t* residual);

}
