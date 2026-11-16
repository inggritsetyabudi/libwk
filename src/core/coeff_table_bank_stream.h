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

#include "coeff_table_stream.h"

namespace wk {

enum class CoeffTableBankMode : uint8_t {
    InlineTables = 0,
    SingleTable = 1,
    PackedNibbleIndices = 2,
    RawByteIndices = 3,
    PackedBitIndices = 4,
    PackedTwoBitIndices = 5,
};

[[nodiscard]] Result<void> write_coefficient_table_bank(ByteWriter& writer,
                                                        std::span<const LossyCoeffTable> tables,
                                                        int num_symbols);
[[nodiscard]] Result<std::vector<LossyCoeffTable>> read_coefficient_table_bank(ByteReader& reader,
                                                                                size_t expected_count,
                                                                                int num_symbols,
                                                                                std::string_view label);

}
