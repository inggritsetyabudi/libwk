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

namespace wk
{

    [[nodiscard]] constexpr size_t packed_coefficient_span_bytes(size_t span_count)
    {
        return (span_count * 7 + 7) / 8;
    }

    [[nodiscard]] uint8_t compute_coefficient_span(const DctBlockI16 &block);
    [[nodiscard]] Result<std::vector<uint8_t>> pack_coefficient_spans(std::span<const uint8_t> spans);
    [[nodiscard]] Result<std::vector<uint8_t>> unpack_coefficient_spans(std::span<const uint8_t> bytes,
                                                                        size_t expected_count,
                                                                        std::string_view label);
    [[nodiscard]] Result<size_t> adaptive_coefficient_span_stream_bytes(std::span<const uint8_t> spans);
    [[nodiscard]] Result<void> write_packed_coefficient_spans(ByteWriter &writer,
                                                              std::span<const uint8_t> spans);
    [[nodiscard]] Result<std::vector<uint8_t>> read_packed_coefficient_spans(ByteReader &reader,
                                                                             size_t expected_count,
                                                                             std::string_view label);
    [[nodiscard]] Result<void> write_adaptive_coefficient_spans(ByteWriter &writer,
                                                                std::span<const uint8_t> spans);
    [[nodiscard]] Result<std::vector<uint8_t>> read_adaptive_coefficient_spans(ByteReader &reader,
                                                                               size_t expected_count,
                                                                               std::string_view label);

}
