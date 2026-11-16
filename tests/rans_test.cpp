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

#include <gtest/gtest.h>
#include "../src/core/rans.h"
#include <random>
#include <numeric>

using namespace wk;

TEST(RansTest, UniformDistribution) {

    constexpr int NUM_SYMBOLS = 8;
    constexpr int NUM_DATA = 1000;

    RansTable<12> table;
    table.build_uniform(NUM_SYMBOLS);


    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, NUM_SYMBOLS - 1);
    std::vector<int> data(NUM_DATA);
    for (auto& d : data) d = dist(rng);


    RansEncoder<12> enc;
    enc.init();
    for (int i = NUM_DATA - 1; i >= 0; i--) {
        enc.encode(table, data[i]);
    }
    auto encoded = enc.finish();

    EXPECT_GT(encoded.size(), 0u);


    RansDecoder<12> dec;
    dec.init(encoded.data(), encoded.size());

    for (int i = 0; i < NUM_DATA; i++) {
        int sym = dec.decode(table);
        EXPECT_EQ(sym, data[i]) << "Mismatch at position " << i;
    }
}

TEST(RansTest, SkewedDistribution) {
    constexpr int NUM_SYMBOLS = 256;
    constexpr int NUM_DATA = 5000;


    uint32_t counts[256] = {};
    counts[0] = 9000;
    counts[1] = 500;
    counts[2] = 300;
    counts[3] = 100;
    for (int i = 4; i < 256; i++) counts[i] = 1;

    RansTable<12> table;
    table.build_from_counts(counts, NUM_SYMBOLS);


    std::mt19937 rng(123);
    std::discrete_distribution<int> dist(counts, counts + 256);
    std::vector<int> data(NUM_DATA);
    for (auto& d : data) d = dist(rng);


    RansEncoder<12> enc;
    enc.init();
    for (int i = NUM_DATA - 1; i >= 0; i--) {
        enc.encode(table, data[i]);
    }
    auto encoded = enc.finish();


    EXPECT_LT(encoded.size(), static_cast<size_t>(NUM_DATA));


    RansDecoder<12> dec;
    dec.init(encoded.data(), encoded.size());

    for (int i = 0; i < NUM_DATA; i++) {
        int sym = dec.decode(table);
        EXPECT_EQ(sym, data[i]) << "Mismatch at position " << i;
    }
}

TEST(RansTest, SingleSymbol) {
    RansTable<12> table;
    uint32_t counts[1] = {1000};
    table.build_from_counts(counts, 1);

    RansEncoder<12> enc;
    enc.init();
    for (int i = 0; i < 100; i++) {
        enc.encode(table, 0);
    }
    auto encoded = enc.finish();

    EXPECT_GT(encoded.size(), 0u);

    RansDecoder<12> dec;
    dec.init(encoded.data(), encoded.size());
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(dec.decode(table), 0);
    }
}

TEST(RansTest, Determinism) {
    constexpr int NUM_SYMBOLS = 16;
    constexpr int NUM_DATA = 500;

    uint32_t counts[16];
    for (int i = 0; i < 16; i++) counts[i] = (i + 1) * 10;

    RansTable<12> table;
    table.build_from_counts(counts, NUM_SYMBOLS);

    std::mt19937 rng(999);
    std::vector<int> data(NUM_DATA);
    for (auto& d : data) d = rng() % NUM_SYMBOLS;


    auto encode_and_get = [&]() {
        RansEncoder<12> enc;
        enc.init();
        for (int i = NUM_DATA - 1; i >= 0; i--) {
            enc.encode(table, data[i]);
        }
        return enc.finish();
    };

    auto encoded1 = encode_and_get();
    auto encoded2 = encode_and_get();


    EXPECT_EQ(encoded1.size(), encoded2.size());
    EXPECT_EQ(encoded1, encoded2);
}

TEST(RansTest, LargeAlphabet) {
    constexpr int NUM_SYMBOLS = 2049;
    constexpr int NUM_DATA = 2000;

    uint32_t counts[2049];
    std::mt19937 rng(77);
    for (int i = 0; i < NUM_SYMBOLS; i++) {
        counts[i] = rng() % 100 + 1;
    }

    RansTable<12> table;
    table.build_from_counts(counts, NUM_SYMBOLS);

    std::discrete_distribution<int> dist(counts, counts + NUM_SYMBOLS);
    std::vector<int> data(NUM_DATA);
    for (auto& d : data) d = dist(rng);

    RansEncoder<12> enc;
    enc.init();
    for (int i = NUM_DATA - 1; i >= 0; i--) {
        enc.encode(table, data[i]);
    }
    auto encoded = enc.finish();

    RansDecoder<12> dec;
    dec.init(encoded.data(), encoded.size());
    for (int i = 0; i < NUM_DATA; i++) {
        EXPECT_EQ(dec.decode(table), data[i]) << "at " << i;
    }
}

TEST(RansTest, FrequencyTableNormalization) {

    constexpr int TABLE_SIZE = 1 << 12;

    uint32_t counts[] = {1, 1, 1, 1000, 500, 200, 50, 10};
    RansTable<12> table;
    table.build_from_counts(counts, 8);

    uint32_t total = 0;
    for (int i = 0; i < 8; i++) {
        total += table.symbol(i).freq;
    }
    EXPECT_EQ(total, TABLE_SIZE);


    for (int i = 0; i < 8; i++) {
        if (counts[i] > 0) {
            EXPECT_GE(table.symbol(i).freq, 1u);
        }
    }
}

TEST(RansTest, CorruptStreamIsRejected) {
    RansTable<12> table;
    table.build_uniform(4);

    std::vector<int> data = {0, 1, 2, 3, 1, 2, 0, 3, 2, 1};
    RansEncoder<12> enc;
    enc.init();
    for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
        enc.encode(table, data[static_cast<size_t>(i)]);
    }
    auto encoded = enc.finish();

    encoded[0] = 0;
    encoded[1] = 0;
    encoded[2] = 0;
    encoded[3] = 0;

    RansDecoder<12> dec;
    dec.init(encoded.data(), encoded.size());
    EXPECT_FALSE(dec.ok());
}

TEST(RansTest, TruncatedStreamIsRejected) {
    RansTable<12> table;
    table.build_uniform(8);

    std::vector<int> data = {0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1};
    RansEncoder<12> enc;
    enc.init();
    for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
        enc.encode(table, data[static_cast<size_t>(i)]);
    }
    auto encoded = enc.finish();
    ASSERT_GT(encoded.size(), 4u);
    encoded.resize(encoded.size() - 1u);

    RansDecoder<12> dec;
    dec.init(encoded.data(), encoded.size());
    ASSERT_TRUE(dec.ok());

    for (size_t i = 0; i < data.size() && dec.ok(); ++i) {
        (void)dec.decode(table);
    }
    EXPECT_FALSE(dec.ok());
}
