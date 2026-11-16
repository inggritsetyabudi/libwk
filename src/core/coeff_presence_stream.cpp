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
#include "coeff_presence_stream.h"

#include <algorithm>

namespace wk {

namespace {

[[nodiscard]] Error invalid_presence_error(std::string_view label, std::string message) {
    return Error{ErrorCode::DecodeFailed,
                 std::string(label) + " coefficient presence " + std::move(message)};
}

[[nodiscard]] bool all_presence_values(std::span<const uint8_t> presence, uint8_t value) {
    return std::all_of(presence.begin(), presence.end(), [&](uint8_t entry) { return entry == value; });
}

}

Result<std::vector<uint8_t>> pack_coefficient_presence(std::span<const uint8_t> presence) {
    const size_t packed_size = packed_coefficient_presence_bytes(presence.size());
    std::vector<uint8_t> packed(packed_size, 0);

    for (size_t index = 0; index < presence.size(); ++index) {
        if (presence[index] > 1) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "coefficient presence value is out of range"});
        }
        packed[index / 8] |= static_cast<uint8_t>(presence[index] << (index % 8));
    }

    return packed;
}

Result<std::vector<uint8_t>> unpack_coefficient_presence(std::span<const uint8_t> bytes,
                                                         size_t expected_count,
                                                         std::string_view label) {
    const size_t expected_bytes = packed_coefficient_presence_bytes(expected_count);
    if (bytes.size() != expected_bytes) {
        return std::unexpected(invalid_presence_error(label, "stream size mismatch"));
    }

    std::vector<uint8_t> presence(expected_count, 0);
    for (size_t index = 0; index < expected_count; ++index) {
        presence[index] = static_cast<uint8_t>((bytes[index / 8] >> (index % 8)) & 1u);
    }

    const size_t used_bits_in_last_byte = expected_count % 8u;
    if (!bytes.empty() && used_bits_in_last_byte != 0u) {
        const uint8_t valid_mask = static_cast<uint8_t>((1u << used_bits_in_last_byte) - 1u);
        if ((bytes.back() & static_cast<uint8_t>(~valid_mask)) != 0) {
            return std::unexpected(invalid_presence_error(label, "stream has non-zero padding"));
        }
    }

    return presence;
}

Result<void> write_adaptive_coefficient_presence(ByteWriter& writer, std::span<const uint8_t> presence) {
    for (uint8_t entry : presence) {
        if (entry > 1) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "coefficient presence value is out of range"});
        }
    }

    if (presence.empty() || all_presence_values(presence, 0)) {
        writer.write_u8(static_cast<uint8_t>(CoeffPresenceEncoding::AllZero));
        return {};
    }
    if (all_presence_values(presence, 1)) {
        writer.write_u8(static_cast<uint8_t>(CoeffPresenceEncoding::AllOne));
        return {};
    }

    auto packed = pack_coefficient_presence(presence);
    if (!packed) {
        return std::unexpected(packed.error());
    }

    writer.write_u8(static_cast<uint8_t>(CoeffPresenceEncoding::RawPacked));
    writer.write_bytes(*packed);
    return {};
}

Result<std::vector<uint8_t>> read_adaptive_coefficient_presence(ByteReader& reader,
                                                                size_t expected_count,
                                                                std::string_view label) {
    auto encoding = reader.read_u8();
    if (!encoding) {
        return std::unexpected(encoding.error());
    }

    switch (static_cast<CoeffPresenceEncoding>(*encoding)) {
    case CoeffPresenceEncoding::AllZero:
        return std::vector<uint8_t>(expected_count, 0);
    case CoeffPresenceEncoding::AllOne:
        return std::vector<uint8_t>(expected_count, 1);
    case CoeffPresenceEncoding::RawPacked: {
        const size_t packed_size = packed_coefficient_presence_bytes(expected_count);
        auto packed = reader.read_bytes(packed_size);
        if (!packed) {
            return std::unexpected(packed.error());
        }
        return unpack_coefficient_presence(*packed, expected_count, label);
    }
    default:
        return std::unexpected(invalid_presence_error(label, "uses an unknown encoding"));
    }
}

}
