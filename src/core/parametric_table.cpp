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
#include "parametric_table.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "coeff_table_stream.h"

namespace wk {

namespace {

constexpr uint16_t kQuantizerScale = 4096;
constexpr uint16_t kRatioScale = 255;

// Magnitude of the fitted geometric tail at magnitude m (m >= 2):
// rest * (1 - r) * r^(m - 2). Written iteratively to avoid pow(0, 0)
// and to keep the reconstruction identical on every platform.
[[nodiscard]] double tail_magnitude(uint32_t rest, double ratio, int magnitude) {
    double value = static_cast<double>(rest) * (1.0 - ratio);
    for (int step = 2; step < magnitude; ++step) {
        value *= ratio;
        if (value == 0.0) {
            break;
        }
    }
    return value;
}

[[nodiscard]] Error invalid_param_error(std::string_view label, std::string message) {
    return Error{ErrorCode::RansError,
                 std::string(label) + " parametric table " + std::move(message)};
}

}

std::optional<ParametricM3> fit_parametric_m3(std::span<const uint32_t> symbol_counts,
                                              int limit, bool split) {
    if (limit < 0) {
        return std::nullopt;
    }
    std::vector<uint64_t> magnitude_counts(static_cast<size_t>(limit) + 1, 0);
    uint64_t total = 0;
    if (split) {
        const size_t count = std::min(symbol_counts.size(), magnitude_counts.size());
        for (size_t magnitude = 0; magnitude < count; ++magnitude) {
            magnitude_counts[magnitude] = symbol_counts[magnitude];
            total += symbol_counts[magnitude];
        }
    } else {
        for (size_t symbol = 0; symbol < symbol_counts.size(); ++symbol) {
            const int magnitude = std::abs(static_cast<int>(symbol) - limit);
            if (magnitude > limit) {
                continue;
            }
            magnitude_counts[static_cast<size_t>(magnitude)] += symbol_counts[symbol];
            total += symbol_counts[symbol];
        }
    }
    if (total == 0) {
        return std::nullopt;
    }

    const uint64_t n0 = magnitude_counts[0];
    const uint64_t n1 = limit >= 1 ? magnitude_counts[1] : 0;
    uint64_t tail_count = 0;
    uint64_t tail_sum = 0;
    for (int magnitude = 2; magnitude <= limit; ++magnitude) {
        const uint64_t count = magnitude_counts[static_cast<size_t>(magnitude)];
        tail_count += count;
        tail_sum += count * static_cast<uint64_t>(magnitude - 1);
    }

    const double z = static_cast<double>(n0) / static_cast<double>(total);
    const double p1 = static_cast<double>(n1) / static_cast<double>(total);
    double ratio = 0.5;
    if (tail_count > 0) {
        const double mean = static_cast<double>(tail_sum) / static_cast<double>(tail_count);
        ratio = std::clamp(1.0 - 1.0 / mean, 0.01, 0.99);
    }

    uint16_t z_q = static_cast<uint16_t>(std::llround(z * kQuantizerScale));
    uint16_t p1_q = static_cast<uint16_t>(std::llround(p1 * kQuantizerScale));
    // Both fractions can round up at once (z_q + p1_q = 4097); keep the
    // quantized pair inside the 4096 budget the reader enforces.
    if (static_cast<uint32_t>(z_q) + p1_q > kQuantizerScale) {
        p1_q -= static_cast<uint16_t>(static_cast<uint32_t>(z_q) + p1_q - kQuantizerScale);
    }
    const uint8_t r_q = static_cast<uint8_t>(std::llround(ratio * kRatioScale));

    uint16_t max_mag = 0;
    for (int magnitude = limit; magnitude >= 0; --magnitude) {
        if (magnitude_counts[static_cast<size_t>(magnitude)] > 0) {
            max_mag = static_cast<uint16_t>(magnitude);
            break;
        }
    }
    return ParametricM3{.z_q = z_q, .p1_q = p1_q, .r_q = r_q, .max_mag = max_mag};
}

RansTable<RANS_PRECISION_BITS> reconstruct_parametric_m3(const ParametricM3& params, int limit, bool split) {
    const int num_symbols = split ? limit + 1 : 2 * limit + 1;
    const double ratio = static_cast<double>(params.r_q) / kRatioScale;
    const uint32_t rest = static_cast<uint32_t>(
        std::max(0, static_cast<int>(kQuantizerScale) - params.z_q - params.p1_q));

    auto fitted_magnitude = [&](int magnitude) -> double {
        if (magnitude == 0) {
            return params.z_q;
        }
        if (magnitude == 1) {
            return limit >= 1 ? params.p1_q : 0.0;
        }
        return tail_magnitude(rest, ratio, magnitude);
    };

    std::vector<uint32_t> counts(static_cast<size_t>(num_symbols), 0);
    auto count_nonzero = [&]() {
        uint32_t nonzero = 0;
        for (uint32_t count : counts) {
            if (count > 0) {
                ++nonzero;
            }
        }
        return nonzero;
    };

    if (split) {
        for (int magnitude = 0; magnitude <= limit; ++magnitude) {
            counts[static_cast<size_t>(magnitude)] =
                static_cast<uint32_t>(std::llround(fitted_magnitude(magnitude)));
        }
        // Floor every magnitude up to max_mag to >= 1 so observed
        // symbols stay covered, stopping at TABLE_SIZE nonzero symbols.
        uint32_t nonzero = count_nonzero();
        for (int magnitude = 0;
             magnitude <= params.max_mag && nonzero < RansTable<RANS_PRECISION_BITS>::TABLE_SIZE;
             ++magnitude) {
            if (counts[static_cast<size_t>(magnitude)] == 0) {
                counts[static_cast<size_t>(magnitude)] = 1;
                ++nonzero;
            }
        }
    } else {
        const int offset = limit;
        for (int magnitude = 0; magnitude <= limit; ++magnitude) {
            const uint32_t half =
                static_cast<uint32_t>(std::llround(fitted_magnitude(magnitude) / 2.0));
            if (magnitude == 0) {
                counts[static_cast<size_t>(offset)] =
                    static_cast<uint32_t>(std::llround(fitted_magnitude(0)));
            } else {
                counts[static_cast<size_t>(offset - magnitude)] = half;
                counts[static_cast<size_t>(offset + magnitude)] = half;
            }
        }
        uint32_t nonzero = count_nonzero();
        if (counts[static_cast<size_t>(offset)] == 0 &&
            nonzero < RansTable<RANS_PRECISION_BITS>::TABLE_SIZE) {
            counts[static_cast<size_t>(offset)] = 1;
            ++nonzero;
        }
        for (int magnitude = 1;
             magnitude <= params.max_mag && nonzero < RansTable<RANS_PRECISION_BITS>::TABLE_SIZE;
             ++magnitude) {
            const size_t lower = static_cast<size_t>(offset - magnitude);
            const size_t upper = static_cast<size_t>(offset + magnitude);
            if (counts[lower] == 0 && counts[upper] == 0) {
                if (nonzero + 2 > RansTable<RANS_PRECISION_BITS>::TABLE_SIZE) {
                    break;
                }
                counts[lower] = 1;
                counts[upper] = 1;
                nonzero += 2;
            }
        }
    }

    RansTable<RANS_PRECISION_BITS> table;
    table.build_from_counts(counts.data(), num_symbols);
    return table;
}

void write_parametric_m3_entry(ByteWriter& writer, const ParametricM3& params) {
    writer.write_u8(static_cast<uint8_t>(CoeffTableEncoding::ParametricM3));
    writer.write_u16(params.z_q);
    writer.write_u16(params.p1_q);
    writer.write_u8(params.r_q);
    writer.write_u16(params.max_mag);
}

Result<ParametricM3> read_parametric_m3_params(ByteReader& reader, int limit,
                                              std::string_view label) {
    auto z_q = reader.read_u16();
    auto p1_q = reader.read_u16();
    auto r_q = reader.read_u8();
    auto max_mag = reader.read_u16();
    if (!z_q || !p1_q || !r_q || !max_mag) {
        return std::unexpected(Error{ErrorCode::TruncatedInput,
                                     std::string(label) + " parametric table is truncated"});
    }
    if (*z_q > kQuantizerScale) {
        return std::unexpected(invalid_param_error(label, "has out-of-range zero fraction"));
    }
    if (*p1_q > kQuantizerScale) {
        return std::unexpected(invalid_param_error(label, "has out-of-range magnitude-1 fraction"));
    }
    if (static_cast<uint32_t>(*z_q) + *p1_q > kQuantizerScale) {
        return std::unexpected(invalid_param_error(label, "has fractions summing above 4096"));
    }
    if (*max_mag > limit) {
        return std::unexpected(invalid_param_error(label, "has out-of-range max magnitude"));
    }
    return ParametricM3{.z_q = *z_q, .p1_q = *p1_q, .r_q = *r_q, .max_mag = *max_mag};
}

}
