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
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace wk {

constexpr int RANS_PRECISION_BITS = 12;
constexpr uint32_t RANS_L = 1u << 23;
constexpr uint32_t RANS_B = 1u << 8;
constexpr uint32_t RANS_UPPER = RANS_L * RANS_B;

struct RansSymbol {
    uint16_t freq = 0;
    uint16_t cum_freq = 0;
};

template<int PrecisionBits = RANS_PRECISION_BITS>
class RansTable {
public:
    static constexpr uint32_t TABLE_SIZE = 1u << PrecisionBits;

    RansTable() = default;

    void build_from_counts(const uint32_t* counts, int num_symbols) {
        num_symbols_ = num_symbols;
        symbols_.assign(num_symbols_, {});
        normalize_frequencies(counts, num_symbols_);

        uint16_t cum = 0;
        for (int i = 0; i < num_symbols_; ++i) {
            symbols_[i].cum_freq = cum;
            cum = static_cast<uint16_t>(cum + symbols_[i].freq);
        }

        build_decode_table();
    }

    void build_uniform(int num_symbols) {
        num_symbols_ = num_symbols;
        symbols_.assign(num_symbols_, {});

        const uint16_t base_freq = static_cast<uint16_t>(TABLE_SIZE / num_symbols_);
        const uint16_t remainder = static_cast<uint16_t>(TABLE_SIZE % num_symbols_);

        uint16_t cum = 0;
        for (int i = 0; i < num_symbols_; ++i) {
            symbols_[i].freq = static_cast<uint16_t>(base_freq + (i < remainder ? 1 : 0));
            symbols_[i].cum_freq = cum;
            cum = static_cast<uint16_t>(cum + symbols_[i].freq);
        }

        build_decode_table();
    }

    [[nodiscard]] const RansSymbol& symbol(int index) const { return symbols_[index]; }
    [[nodiscard]] int num_symbols() const { return num_symbols_; }

    [[nodiscard]] int lookup(uint16_t cum_freq) const {
        if (cum_freq >= decode_table_.size()) {
            return 0;
        }
        return decode_table_[cum_freq];
    }

private:
    void normalize_frequencies(const uint32_t* counts, int count_size) {
        uint64_t total = 0;
        int max_index = 0;
        uint32_t max_count = 0;
        std::vector<int> non_zero;
        non_zero.reserve(count_size);

        for (int i = 0; i < count_size; ++i) {
            total += counts[i];
            if (counts[i] > 0) {
                non_zero.push_back(i);
                if (counts[i] > max_count) {
                    max_count = counts[i];
                    max_index = i;
                }
            }
        }

        if (total == 0 || non_zero.empty()) {
            build_uniform(count_size);
            return;
        }

        std::vector<double> exact(count_size, 0.0);
        uint32_t assigned = 0;
        for (int i : non_zero) {
            exact[i] = (static_cast<double>(counts[i]) * static_cast<double>(TABLE_SIZE)) /
                       static_cast<double>(total);
            uint32_t freq = static_cast<uint32_t>(std::floor(exact[i]));
            if (freq == 0) {
                freq = 1;
            }
            symbols_[i].freq = static_cast<uint16_t>(freq);
            assigned += freq;
        }

        while (assigned > TABLE_SIZE) {
            int best = -1;
            double best_score = -std::numeric_limits<double>::infinity();
            for (int i : non_zero) {
                if (symbols_[i].freq <= 1) {
                    continue;
                }
                const double score = static_cast<double>(symbols_[i].freq) - exact[i];
                if (score > best_score) {
                    best_score = score;
                    best = i;
                }
            }
            if (best < 0) {
                best = max_index;
                if (symbols_[best].freq <= 1) {
                    break;
                }
            }
            --symbols_[best].freq;
            --assigned;
        }

        while (assigned < TABLE_SIZE) {
            int best = max_index;
            double best_score = -std::numeric_limits<double>::infinity();
            for (int i : non_zero) {
                const double score = exact[i] - static_cast<double>(symbols_[i].freq);
                if (score > best_score) {
                    best_score = score;
                    best = i;
                }
            }
            ++symbols_[best].freq;
            ++assigned;
        }
    }

    void build_decode_table() {
        decode_table_.assign(TABLE_SIZE, 0);
        for (int symbol_index = 0; symbol_index < num_symbols_; ++symbol_index) {
            const auto& symbol = symbols_[symbol_index];
            for (uint16_t offset = 0; offset < symbol.freq; ++offset) {
                decode_table_[symbol.cum_freq + offset] = symbol_index;
            }
        }
    }

    int num_symbols_ = 0;
    std::vector<RansSymbol> symbols_;
    std::vector<int> decode_table_;
};

template<int PrecisionBits = RANS_PRECISION_BITS>
class RansEncoder {
public:
    static constexpr uint32_t M = 1u << PrecisionBits;

    RansEncoder() { init(); }

    void init() {
        state_ = RANS_L;
        emitted_.clear();
    }

    void encode(uint16_t freq, uint16_t cum_freq) {
        if (freq == 0) {
            return;
        }

        const uint64_t x_max = (static_cast<uint64_t>(RANS_L >> PrecisionBits) << 8) * freq;
        while (state_ >= x_max) {
            emitted_.push_back(static_cast<uint8_t>(state_ & 0xFFu));
            state_ >>= 8;
        }

        state_ = ((state_ / freq) << PrecisionBits) + (state_ % freq) + cum_freq;
    }

    void encode(const RansSymbol& symbol) {
        encode(symbol.freq, symbol.cum_freq);
    }

    void encode(const RansTable<PrecisionBits>& table, int symbol) {
        encode(table.symbol(symbol));
    }

    [[nodiscard]] std::vector<uint8_t> finish() const {
        std::vector<uint8_t> stream;
        stream.reserve(4 + emitted_.size());
        stream.push_back(static_cast<uint8_t>(state_ & 0xFFu));
        stream.push_back(static_cast<uint8_t>((state_ >> 8) & 0xFFu));
        stream.push_back(static_cast<uint8_t>((state_ >> 16) & 0xFFu));
        stream.push_back(static_cast<uint8_t>((state_ >> 24) & 0xFFu));
        for (auto it = emitted_.rbegin(); it != emitted_.rend(); ++it) {
            stream.push_back(*it);
        }
        return stream;
    }

private:
    uint32_t state_ = RANS_L;
    std::vector<uint8_t> emitted_;
};

template<int PrecisionBits = RANS_PRECISION_BITS>
class RansDecoder {
public:
    static constexpr uint32_t M = 1u << PrecisionBits;

    void init(const uint8_t* data, size_t size) {
        data_ = data;
        size_ = size;
        pos_ = 0;
        ok_ = data_ != nullptr && size_ >= 4;
        if (!ok_) {
            state_ = 0;
            return;
        }

        state_ = static_cast<uint32_t>(data_[0]) |
                 (static_cast<uint32_t>(data_[1]) << 8) |
                 (static_cast<uint32_t>(data_[2]) << 16) |
                 (static_cast<uint32_t>(data_[3]) << 24);
        pos_ = 4;
        ok_ = state_ >= RANS_L && state_ < RANS_UPPER;
    }

    [[nodiscard]] int decode(const RansTable<PrecisionBits>& table) {
        if (!ok_) {
            return 0;
        }

        const uint16_t cum_freq = static_cast<uint16_t>(state_ & (M - 1));
        const int symbol_index = table.lookup(cum_freq);
        const auto& symbol = table.symbol(symbol_index);
        if (symbol.freq == 0 || cum_freq < symbol.cum_freq ||
            cum_freq >= symbol.cum_freq + symbol.freq) {
            ok_ = false;
            return 0;
        }

        state_ = symbol.freq * (state_ >> PrecisionBits) + (cum_freq - symbol.cum_freq);
        while (state_ < RANS_L) {
            if (pos_ >= size_) {
                ok_ = false;
                return symbol_index;
            }
            state_ = (state_ << 8) | data_[pos_++];
        }

        return symbol_index;
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] size_t position() const { return pos_; }

private:
    uint32_t state_ = 0;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool ok_ = false;
};

inline uint32_t pixel_context_hash(const uint8_t* above, const uint8_t* left,
                                    const uint8_t* above_left, int channel) {
    uint32_t hash = 0;
    if (above) {
        hash ^= static_cast<uint32_t>(above[channel]) * 2654435761u;
    }
    if (left) {
        hash ^= static_cast<uint32_t>(left[channel]) * 2246822519u;
    }
    if (above_left) {
        hash ^= static_cast<uint32_t>(above_left[channel]) * 3266489917u;
    }
    return hash;
}

inline int ac_context(int zigzag_index, int plane) {
    return plane * 64 + zigzag_index;
}
}
