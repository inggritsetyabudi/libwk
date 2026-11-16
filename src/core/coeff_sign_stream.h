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

constexpr uint8_t kCoefficientSignModeRawPacked = 0;
constexpr uint8_t kCoefficientSignModeAllPositive = 1;
constexpr uint8_t kCoefficientSignModeAllNegative = 2;

[[nodiscard]] constexpr size_t packed_coefficient_sign_bytes(size_t sign_count) {
    return (sign_count + 7) / 8;
}

[[nodiscard]] constexpr size_t packed_coefficient_sign_mode_bytes(size_t mode_count) {
    return (mode_count + 3) / 4;
}

[[nodiscard]] Result<std::vector<uint8_t>> pack_coefficient_signs(std::span<const uint8_t> signs);
[[nodiscard]] Result<std::vector<uint8_t>> unpack_coefficient_signs(std::span<const uint8_t> bytes,
                                                                    size_t expected_count,
                                                                    std::string_view label);
[[nodiscard]] Result<std::vector<uint8_t>> pack_coefficient_sign_modes(std::span<const uint8_t> modes);
[[nodiscard]] Result<std::vector<uint8_t>> unpack_coefficient_sign_modes(std::span<const uint8_t> bytes,
                                                                         size_t expected_count,
                                                                         std::string_view label);
[[nodiscard]] Result<std::vector<uint8_t>> read_packed_coefficient_signs(ByteReader& reader,
                                                                         size_t expected_count,
                                                                         std::string_view label);

}
