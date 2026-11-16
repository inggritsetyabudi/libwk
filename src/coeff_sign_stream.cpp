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
#include "coeff_sign_stream.h"

namespace wk {

namespace {

[[nodiscard]] Error invalid_sign_error(std::string_view label, uint8_t sign_value) {
    return Error{ErrorCode::DecodeFailed,
                 std::string("invalid ") + std::string(label) +
                     " coefficient sign value " + std::to_string(sign_value)};
}

[[nodiscard]] Error invalid_sign_mode_error(std::string_view label, uint8_t mode_value) {
    return Error{ErrorCode::DecodeFailed,
                 std::string("invalid ") + std::string(label) +
                     " coefficient sign mode " + std::to_string(mode_value)};
}

}

Result<std::vector<uint8_t>> pack_coefficient_signs(std::span<const uint8_t> signs) {
    const size_t packed_size = packed_coefficient_sign_bytes(signs.size());
    std::vector<uint8_t> packed(packed_size, 0);

    for (size_t i = 0; i < signs.size(); ++i) {
        if (signs[i] > 1) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "coefficient sign value is out of range"});
        }
        packed[i / 8] |= static_cast<uint8_t>(signs[i] << (i % 8));
    }

    return packed;
}

Result<std::vector<uint8_t>> unpack_coefficient_signs(std::span<const uint8_t> bytes,
                                                      size_t expected_count,
                                                      std::string_view label) {
    const size_t expected_bytes = packed_coefficient_sign_bytes(expected_count);
    if (bytes.size() != expected_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecodeFailed,
                  std::string(label) + " coefficient sign stream size mismatch"});
    }

    std::vector<uint8_t> signs(expected_count, 0);
    for (size_t i = 0; i < expected_count; ++i) {
        const uint8_t sign_value = static_cast<uint8_t>((bytes[i / 8] >> (i % 8)) & 1u);
        if (sign_value > 1) {
            return std::unexpected(invalid_sign_error(label, sign_value));
        }
        signs[i] = sign_value;
    }

    const size_t used_bits_in_last_byte = expected_count % 8;
    if (!bytes.empty() && used_bits_in_last_byte != 0) {
        const uint8_t valid_mask = static_cast<uint8_t>((1u << used_bits_in_last_byte) - 1u);
        if ((bytes.back() & static_cast<uint8_t>(~valid_mask)) != 0) {
            return std::unexpected(
                Error{ErrorCode::DecodeFailed,
                      std::string(label) + " coefficient sign stream has non-zero padding"});
        }
    }

    return signs;
}

Result<std::vector<uint8_t>> pack_coefficient_sign_modes(std::span<const uint8_t> modes) {
    const size_t packed_size = packed_coefficient_sign_mode_bytes(modes.size());
    std::vector<uint8_t> packed(packed_size, 0);

    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i] > kCoefficientSignModeAllNegative) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "coefficient sign mode is out of range"});
        }
        packed[i / 4] |= static_cast<uint8_t>(modes[i] << ((i % 4) * 2));
    }

    return packed;
}

Result<std::vector<uint8_t>> unpack_coefficient_sign_modes(std::span<const uint8_t> bytes,
                                                           size_t expected_count,
                                                           std::string_view label) {
    const size_t expected_bytes = packed_coefficient_sign_mode_bytes(expected_count);
    if (bytes.size() != expected_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecodeFailed,
                  std::string(label) + " coefficient sign mode stream size mismatch"});
    }

    std::vector<uint8_t> modes(expected_count, kCoefficientSignModeRawPacked);
    for (size_t i = 0; i < expected_count; ++i) {
        const uint8_t mode_value = static_cast<uint8_t>((bytes[i / 4] >> ((i % 4) * 2)) & 0x3u);
        if (mode_value > kCoefficientSignModeAllNegative) {
            return std::unexpected(invalid_sign_mode_error(label, mode_value));
        }
        modes[i] = mode_value;
    }

    const size_t used_bits_in_last_byte = (expected_count % 4) * 2;
    if (!bytes.empty() && used_bits_in_last_byte != 0) {
        const uint8_t valid_mask = static_cast<uint8_t>((1u << used_bits_in_last_byte) - 1u);
        if ((bytes.back() & static_cast<uint8_t>(~valid_mask)) != 0) {
            return std::unexpected(
                Error{ErrorCode::DecodeFailed,
                      std::string(label) + " coefficient sign mode stream has non-zero padding"});
        }
    }

    return modes;
}

Result<std::vector<uint8_t>> read_packed_coefficient_signs(ByteReader& reader,
                                                           size_t expected_count,
                                                           std::string_view label) {
    const size_t packed_size = packed_coefficient_sign_bytes(expected_count);
    auto packed_bytes = reader.read_bytes(packed_size);
    if (!packed_bytes) {
        return std::unexpected(packed_bytes.error());
    }
    return unpack_coefficient_signs(*packed_bytes, expected_count, label);
}

}
