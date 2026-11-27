// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0

#include "true_cost_model.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace wk {
namespace {

float sym_cost(uint32_t count, uint32_t total) {
    if (count > 0) {
        return -std::log2(static_cast<float>(count) / static_cast<float>(total));
    }
    return -std::log2(0.5f / static_cast<float>(total));
}

}  // namespace

void TrueCostModel::build(const std::vector<PredMode>& modes,
                          const std::vector<uint8_t>& spans,
                          const std::vector<DctBlockI16>& blocks,
                          float shrink_beta, float mode_span_beta, float dc_beta) {
    const uint32_t n = static_cast<uint32_t>(blocks.size());
    const uint32_t total = n == 0 ? 1 : n;
    uint32_t mode_cnt[kNumModes] = {};
    uint32_t span_cnt[kMaxSpan] = {};
    for (PredMode m : modes) mode_cnt[static_cast<int>(m)]++;
    for (uint8_t s : spans) span_cnt[s]++;
    // #rdo: mode/span shrinkage toward the uniform prior (iterasi 1).
    // With beta 0 this is exactly v1.
    for (int i = 0; i < kNumModes; ++i) {
        const float p_emp = static_cast<float>(mode_cnt[i]) / static_cast<float>(total);
        const float p = (1.0f - mode_span_beta) * p_emp +
                        mode_span_beta * (1.0f / static_cast<float>(kNumModes));
        mode_cost[i] = p > 0.0f ? -std::log2(p) : sym_cost(0, total);
    }
    for (int i = 0; i < kMaxSpan; ++i) {
        const float p_emp = static_cast<float>(span_cnt[i]) / static_cast<float>(total);
        const float p = (1.0f - mode_span_beta) * p_emp +
                        mode_span_beta * (1.0f / static_cast<float>(kMaxSpan));
        span_cost[i] = p > 0.0f ? -std::log2(p) : sym_cost(0, total);
    }

    // #rdo: per-band pooled magnitude prior (bands over zigzag positions).
    // band_of(pos): {0}, {1..3}, {4..7}, {8..15}, {16..31}, {32..63}.
    static constexpr int kBandStart[6] = {0, 1, 4, 8, 16, 32};
    auto band_of = [](int pos) {
        int b = 0;
        for (int i = 1; i < 6; ++i) {
            if (pos >= kBandStart[i]) b = i;
        }
        return b;
    };
    std::map<int, uint32_t> band_cnt[6];
    uint32_t band_active[6] = {};
    for (size_t b = 0; b < blocks.size(); ++b) {
        for (int i = 0; i < 64; ++i) {
            if (spans[b] <= i) continue;
            const int band = band_of(i);
            ++band_active[band];
            band_cnt[band][std::abs(static_cast<int>(blocks[b][static_cast<size_t>(i)]))]++;
        }
    }

    for (int i = 0; i < 64; ++i) {
        std::map<int, uint32_t> cnt;
        uint32_t active = 0;
        for (size_t b = 0; b < blocks.size(); ++b) {
            if (spans[b] <= i) continue;
            ++active;
            cnt[std::abs(static_cast<int>(blocks[b][static_cast<size_t>(i)]))]++;
        }
        unseen_mag_cost[static_cast<size_t>(i)] =
            -std::log2(0.5f / static_cast<float>(active == 0 ? 1 : active));
        int cap = 0;
        for (const auto& kv : cnt) cap = std::max(cap, kv.first);
        int band_cap = 0;
        const int band = band_of(i);
        for (const auto& kv : band_cnt[band]) band_cap = std::max(band_cap, kv.first);
        const int table_cap = shrink_beta > 0.0f ? std::max(cap, band_cap) : cap;
        auto& table = mag_cost[static_cast<size_t>(i)];
        table.assign(static_cast<size_t>(table_cap) + 1, unseen_mag_cost[static_cast<size_t>(i)]);
        for (int v = 0; v <= table_cap; ++v) {
            const float p_emp = cnt.count(v) == 0
                                    ? 0.0f
                                    : static_cast<float>(cnt[v]) / static_cast<float>(active);
            float p = p_emp;
            if (shrink_beta > 0.0f && band_active[band] > 0) {
                const float p_band =
                    band_cnt[band].count(v) == 0
                        ? 0.0f
                        : static_cast<float>(band_cnt[band][v]) /
                              static_cast<float>(band_active[band]);
                p = (1.0f - shrink_beta) * p_emp + shrink_beta * p_band;
            }
            // #rdo: DC (position 0) shrinkage toward a flat prior over
            // the observed DC range — its band is just itself, so the
            // band prior above is a no-op for DC.
            if (i == 0 && dc_beta > 0.0f) {
                const float p_flat = 1.0f / static_cast<float>(table_cap + 1);
                p = (1.0f - dc_beta) * p + dc_beta * p_flat;
            }
            if (p > 0.0f) table[static_cast<size_t>(v)] = -std::log2(p);
        }
    }
}

float TrueCostModel::coeff_bits(const DctBlockI16& q, uint8_t span) const {
    float c = 0.0f;
    for (int i = 0; i < span; ++i) {
        const size_t pos = static_cast<size_t>(i);
        const int v = q[pos];
        const int a = std::abs(v);
        const auto& table = mag_cost[pos];
        c += (a < static_cast<int>(table.size())) ? table[static_cast<size_t>(a)]
                                                  : unseen_mag_cost[pos];
        if (v != 0) c += 1.0f;  // split sign: 1 bit per nonzero
    }
    return c;
}

float TrueCostModel::block_bits(const DctBlockI16& q, PredMode mode) const {
    const uint8_t span = compute_coefficient_span(q);
    return mode_cost[static_cast<int>(mode)] + span_cost[span] + coeff_bits(q, span);
}

}  // namespace wk
