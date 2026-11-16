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

namespace wk {

enum class CoeffTableEncoding : uint8_t {
    SingleSymbol = 0,
    DenseRange = 1,
    SparsePairs = 2,
    DenseRangeU8 = 3,
    SparsePairsU8 = 4,
    ReusePrevious = 5,
    DeltaSparseI8 = 6,
};

using LossyCoeffTable = RansTable<RANS_PRECISION_BITS>;

[[nodiscard]] Result<void> write_coefficient_table(ByteWriter& writer,
                                                   const LossyCoeffTable& table,
                                                   int num_symbols,
                                                   const LossyCoeffTable* previous_table = nullptr);
[[nodiscard]] Result<LossyCoeffTable> read_coefficient_table(ByteReader& reader,
                                                             int num_symbols,
                                                             std::string_view label,
                                                             const LossyCoeffTable* previous_table = nullptr);

}
