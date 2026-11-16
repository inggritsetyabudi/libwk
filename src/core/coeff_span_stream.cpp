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
#include "coeff_span_stream.h"

#include <limits>

namespace wk {

namespace {

constexpr uint16_t kCoeffSpanEncodingMask = 0xC000u;
constexpr uint16_t kCoeffSpanPayloadMask = 0x3FFFu;
constexpr uint16_t kCoeffSpanEncodingRawPacked = 0x0000u;
constexpr uint16_t kCoeffSpanEncodingSingleValue = 0x4000u;
constexpr uint16_t kCoeffSpanEncodingRunLength = 0x8000u;

struct AdaptiveSpanStream {
    uint16_t header = 0;
    std::vector<uint8_t> payload;
};

[[nodiscard]] Error invalid_span_error(std::string_view label, uint8_t span_value) {
    return Error{ErrorCode::DecodeFailed,
                 std::string("invalid ") + std::string(label) +
                     " coefficient span value " + std::to_string(span_value)};
}

[[nodiscard]] Error invalid_span_stream_error(std::string_view label, std::string message) {
    return Error{ErrorCode::DecodeFailed,
                 std::string(label) + " coefficient span stream " + std::move(message)};
}

[[nodiscard]] Result<void> validate_spans(std::span<const uint8_t> spans) {
    for (uint8_t span_value : spans) {
        if (span_value > 64) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "coefficient span value is out of range"});
        }
    }
    return {};
}

[[nodiscard]] Result<std::vector<uint8_t>> pack_rle_coefficient_spans(std::span<const uint8_t> spans) {
    auto spans_valid = validate_spans(spans);
    if (!spans_valid) {
        return std::unexpected(spans_valid.error());
    }

    if (spans.empty()) {
        return std::vector<uint8_t>{};
    }

    ByteWriter writer;
    size_t index = 0;
    while (index < spans.size()) {
        const uint8_t value = spans[index];
        size_t run_length = 1;
        while (index + run_length < spans.size() &&
               spans[index + run_length] == value &&
               run_length < std::numeric_limits<uint16_t>::max()) {
            ++run_length;
        }
        writer.write_u16(static_cast<uint16_t>(run_length));
        writer.write_u8(value);
        index += run_length;
    }

    return writer.finish();
}

[[nodiscard]] Result<AdaptiveSpanStream> encode_adaptive_coefficient_spans(std::span<const uint8_t> spans) {
    auto spans_valid = validate_spans(spans);
    if (!spans_valid) {
        return std::unexpected(spans_valid.error());
    }

    const size_t raw_payload_size = packed_coefficient_span_bytes(spans.size());
    if (raw_payload_size > kCoeffSpanPayloadMask) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     "packed coefficient span stream is too large"});
    }

    if (!spans.empty()) {
        const uint8_t single_value = spans.front();
        bool all_same = true;
        for (size_t i = 1; i < spans.size(); ++i) {
            if (spans[i] != single_value) {
                all_same = false;
                break;
            }
        }
        if (all_same) {
            AdaptiveSpanStream encoded;
            encoded.header = static_cast<uint16_t>(kCoeffSpanEncodingSingleValue | single_value);
            return encoded;
        }
    }

    auto raw_payload = pack_coefficient_spans(spans);
    if (!raw_payload) {
        return std::unexpected(raw_payload.error());
    }

    auto rle_payload = pack_rle_coefficient_spans(spans);
    if (!rle_payload) {
        return std::unexpected(rle_payload.error());
    }
    if (rle_payload->size() <= kCoeffSpanPayloadMask &&
        rle_payload->size() < raw_payload->size()) {
        AdaptiveSpanStream encoded;
        encoded.header = static_cast<uint16_t>(kCoeffSpanEncodingRunLength | rle_payload->size());
        encoded.payload = std::move(*rle_payload);
        return encoded;
    }

    AdaptiveSpanStream encoded;
    encoded.header = static_cast<uint16_t>(kCoeffSpanEncodingRawPacked | raw_payload->size());
    encoded.payload = std::move(*raw_payload);
    return encoded;
}

}

uint8_t compute_coefficient_span(const DctBlockI16& block) {
    for (int i = 63; i >= 0; --i) {
        if (block[i] != 0) {
            return static_cast<uint8_t>(i + 1);
        }
    }
    return 0;
}

Result<std::vector<uint8_t>> pack_coefficient_spans(std::span<const uint8_t> spans) {
    const size_t packed_size = packed_coefficient_span_bytes(spans.size());
    if (packed_size > std::numeric_limits<uint16_t>::max()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter,
                                     "packed coefficient span stream is too large"});
    }

    std::vector<uint8_t> packed(packed_size, 0);
    size_t bit_offset = 0;
    for (uint8_t span_value : spans) {
        if (span_value > 64) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "coefficient span value is out of range"});
        }

        const size_t byte_index = bit_offset / 8;
        const size_t shift = bit_offset % 8;
        const uint16_t bits = static_cast<uint16_t>(span_value) << shift;
        packed[byte_index] |= static_cast<uint8_t>(bits & 0xFFu);
        if (byte_index + 1 < packed.size()) {
            packed[byte_index + 1] |= static_cast<uint8_t>(bits >> 8);
        }
        bit_offset += 7;
    }

    return packed;
}

Result<std::vector<uint8_t>> unpack_coefficient_spans(std::span<const uint8_t> bytes,
                                                      size_t expected_count,
                                                      std::string_view label) {
    const size_t expected_bytes = packed_coefficient_span_bytes(expected_count);
    if (bytes.size() != expected_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecodeFailed,
                  std::string(label) + " coefficient span stream size mismatch"});
    }

    std::vector<uint8_t> spans(expected_count, 0);
    size_t bit_offset = 0;
    for (size_t i = 0; i < expected_count; ++i) {
        const size_t byte_index = bit_offset / 8;
        const size_t shift = bit_offset % 8;
        uint16_t bits = bytes[byte_index];
        if (byte_index + 1 < bytes.size()) {
            bits |= static_cast<uint16_t>(bytes[byte_index + 1]) << 8;
        }

        const uint8_t span_value = static_cast<uint8_t>((bits >> shift) & 0x7Fu);
        if (span_value > 64) {
            return std::unexpected(invalid_span_error(label, span_value));
        }
        spans[i] = span_value;
        bit_offset += 7;
    }

    const size_t used_bits_in_last_byte = (expected_count * 7) % 8;
    if (!bytes.empty() && used_bits_in_last_byte != 0) {
        const uint8_t valid_mask = static_cast<uint8_t>((1u << used_bits_in_last_byte) - 1u);
        if ((bytes.back() & static_cast<uint8_t>(~valid_mask)) != 0) {
            return std::unexpected(
                Error{ErrorCode::DecodeFailed,
                      std::string(label) + " coefficient span stream has non-zero padding"});
        }
    }

    return spans;
}

Result<size_t> adaptive_coefficient_span_stream_bytes(std::span<const uint8_t> spans) {
    auto encoded = encode_adaptive_coefficient_spans(spans);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return sizeof(uint16_t) + encoded->payload.size();
}

Result<void> write_packed_coefficient_spans(ByteWriter& writer, std::span<const uint8_t> spans) {
    auto packed = pack_coefficient_spans(spans);
    if (!packed) {
        return std::unexpected(packed.error());
    }

    writer.write_u16(static_cast<uint16_t>(packed->size()));
    writer.write_bytes(*packed);
    return {};
}

Result<std::vector<uint8_t>> read_packed_coefficient_spans(ByteReader& reader,
                                                           size_t expected_count,
                                                           std::string_view label) {
    auto packed_size = reader.read_u16();
    if (!packed_size) {
        return std::unexpected(packed_size.error());
    }

    const size_t expected_bytes = packed_coefficient_span_bytes(expected_count);
    if (*packed_size != expected_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecodeFailed,
                  std::string(label) + " coefficient span stream size mismatch"});
    }

    auto packed_bytes = reader.read_bytes(*packed_size);
    if (!packed_bytes) {
        return std::unexpected(packed_bytes.error());
    }

    return unpack_coefficient_spans(*packed_bytes, expected_count, label);
}

Result<void> write_adaptive_coefficient_spans(ByteWriter& writer, std::span<const uint8_t> spans) {
    auto encoded = encode_adaptive_coefficient_spans(spans);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }

    writer.write_u16(encoded->header);
    writer.write_bytes(encoded->payload);
    return {};
}

Result<std::vector<uint8_t>> read_adaptive_coefficient_spans(ByteReader& reader,
                                                             size_t expected_count,
                                                             std::string_view label) {
    auto header = reader.read_u16();
    if (!header) {
        return std::unexpected(header.error());
    }

    const uint16_t encoding = static_cast<uint16_t>(*header & kCoeffSpanEncodingMask);
    const uint16_t payload = static_cast<uint16_t>(*header & kCoeffSpanPayloadMask);
    switch (encoding) {
    case kCoeffSpanEncodingRawPacked: {
        const size_t expected_bytes = packed_coefficient_span_bytes(expected_count);
        if (payload != expected_bytes) {
            return std::unexpected(
                invalid_span_stream_error(label, "size mismatch"));
        }
        auto packed_bytes = reader.read_bytes(payload);
        if (!packed_bytes) {
            return std::unexpected(packed_bytes.error());
        }
        return unpack_coefficient_spans(*packed_bytes, expected_count, label);
    }
    case kCoeffSpanEncodingSingleValue: {
        if ((payload & ~0x7Fu) != 0) {
            return std::unexpected(
                invalid_span_stream_error(label, "has invalid single-value header"));
        }
        const uint8_t span_value = static_cast<uint8_t>(payload);
        if (span_value > 64) {
            return std::unexpected(invalid_span_error(label, span_value));
        }
        return std::vector<uint8_t>(expected_count, span_value);
    }
    case kCoeffSpanEncodingRunLength: {
        if (payload == 0 || (payload % 3u) != 0) {
            return std::unexpected(
                invalid_span_stream_error(label, "has invalid run-length payload size"));
        }
        auto encoded_bytes = reader.read_bytes(payload);
        if (!encoded_bytes) {
            return std::unexpected(encoded_bytes.error());
        }

        std::vector<uint8_t> spans;
        spans.reserve(expected_count);
        ByteReader encoded_reader(*encoded_bytes);
        while (!encoded_reader.at_end()) {
            auto run_length = encoded_reader.read_u16();
            auto span_value = encoded_reader.read_u8();
            if (!run_length || !span_value) {
                return std::unexpected(
                    invalid_span_stream_error(label, "is truncated"));
            }
            if (*run_length == 0) {
                return std::unexpected(
                    invalid_span_stream_error(label, "contains a zero-length run"));
            }
            if (*span_value > 64) {
                return std::unexpected(invalid_span_error(label, *span_value));
            }
            if (spans.size() + *run_length > expected_count) {
                return std::unexpected(
                    invalid_span_stream_error(label, "expands past the expected span count"));
            }
            spans.insert(spans.end(), *run_length, *span_value);
        }

        if (spans.size() != expected_count) {
            return std::unexpected(
                invalid_span_stream_error(label, "does not reconstruct the expected span count"));
        }
        return spans;
    }
    default:
        return std::unexpected(
            invalid_span_stream_error(label, "uses an unknown encoding"));
    }
}

}
