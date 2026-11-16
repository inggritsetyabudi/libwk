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

namespace wk {

enum class CoeffPresenceEncoding : uint8_t {
    AllZero = 0,
    AllOne = 1,
    RawPacked = 2,
};

[[nodiscard]] constexpr size_t packed_coefficient_presence_bytes(size_t count) {
    return (count + 7) / 8;
}

[[nodiscard]] Result<std::vector<uint8_t>> pack_coefficient_presence(std::span<const uint8_t> presence);
[[nodiscard]] Result<std::vector<uint8_t>> unpack_coefficient_presence(std::span<const uint8_t> bytes,
                                                                       size_t expected_count,
                                                                       std::string_view label);
[[nodiscard]] Result<void> write_adaptive_coefficient_presence(ByteWriter& writer,
                                                               std::span<const uint8_t> presence);
[[nodiscard]] Result<std::vector<uint8_t>> read_adaptive_coefficient_presence(ByteReader& reader,
                                                                              size_t expected_count,
                                                                              std::string_view label);

}
