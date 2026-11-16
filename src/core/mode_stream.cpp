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
#include "mode_stream.h"

#include <limits>

namespace wk
{

    namespace
    {

        [[nodiscard]] Error invalid_mode_error(std::string_view label, uint8_t mode_value)
        {
            return Error{ErrorCode::PredictionError,
                         std::string("invalid ") + std::string(label) +
                             " prediction mode value " + std::to_string(mode_value)};
        }

    }

    Result<std::vector<uint8_t>> pack_prediction_modes(std::span<const PredMode> modes)
    {
        const size_t packed_size = packed_prediction_mode_bytes(modes.size());
        if (packed_size > std::numeric_limits<uint16_t>::max())
        {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "packed prediction mode stream is too large"});
        }

        std::vector<uint8_t> packed(packed_size, 0);
        for (size_t i = 0; i < modes.size(); ++i)
        {
            const uint8_t mode_value = static_cast<uint8_t>(modes[i]);
            if (mode_value >= static_cast<uint8_t>(PredMode::NUM_MODES))
            {
                return std::unexpected(
                    Error{ErrorCode::InvalidParameter, "prediction mode value is out of range"});
            }

            const size_t byte_index = i / 2;
            if ((i & 1u) == 0u)
            {
                packed[byte_index] = mode_value;
            }
            else
            {
                packed[byte_index] |= static_cast<uint8_t>(mode_value << 4);
            }
        }

        return packed;
    }

    Result<std::vector<PredMode>> unpack_prediction_modes(std::span<const uint8_t> bytes,
                                                          size_t expected_count,
                                                          std::string_view label)
    {
        const size_t expected_bytes = packed_prediction_mode_bytes(expected_count);
        if (bytes.size() != expected_bytes)
        {
            return std::unexpected(
                Error{ErrorCode::PredictionError,
                      std::string(label) + " prediction mode stream size mismatch"});
        }

        std::vector<PredMode> modes(expected_count);
        size_t mode_index = 0;
        for (uint8_t byte : bytes)
        {
            const uint8_t low = static_cast<uint8_t>(byte & 0x0Fu);
            const uint8_t high = static_cast<uint8_t>((byte >> 4) & 0x0Fu);

            if (mode_index < expected_count)
            {
                if (low >= static_cast<uint8_t>(PredMode::NUM_MODES))
                {
                    return std::unexpected(invalid_mode_error(label, low));
                }
                modes[mode_index++] = static_cast<PredMode>(low);
            }

            if (mode_index < expected_count)
            {
                if (high >= static_cast<uint8_t>(PredMode::NUM_MODES))
                {
                    return std::unexpected(invalid_mode_error(label, high));
                }
                modes[mode_index++] = static_cast<PredMode>(high);
            }
            else if (high != 0)
            {
                return std::unexpected(
                    Error{ErrorCode::PredictionError,
                          std::string(label) + " prediction mode stream has non-zero padding"});
            }
        }

        return modes;
    }

    Result<void> write_packed_prediction_modes(ByteWriter &writer, std::span<const PredMode> modes)
    {
        auto packed = pack_prediction_modes(modes);
        if (!packed)
        {
            return std::unexpected(packed.error());
        }

        writer.write_u16(static_cast<uint16_t>(packed->size()));
        writer.write_bytes(*packed);
        return {};
    }

    Result<std::vector<PredMode>> read_packed_prediction_modes(ByteReader &reader,
                                                               size_t expected_count,
                                                               std::string_view label)
    {
        auto packed_size = reader.read_u16();
        if (!packed_size)
        {
            return std::unexpected(packed_size.error());
        }

        const size_t expected_bytes = packed_prediction_mode_bytes(expected_count);
        if (*packed_size != expected_bytes)
        {
            return std::unexpected(
                Error{ErrorCode::PredictionError,
                      std::string(label) + " prediction mode stream size mismatch"});
        }

        auto packed_bytes = reader.read_bytes(*packed_size);
        if (!packed_bytes)
        {
            return std::unexpected(packed_bytes.error());
        }

        return unpack_prediction_modes(*packed_bytes, expected_count, label);
    }

}
