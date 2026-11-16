// CCLM implementation. See cclm.h.
#include "cclm.h"

#include <algorithm>

namespace wk {
namespace {

// Slope clamp: [-2, 2] in Q8.
constexpr int32_t kMaxSlopeQ8 = 512;

}  // namespace

CclmParams derive_cclm_params(const int16_t* luma_above,
                              const int16_t* chroma_above,
                              const int16_t* luma_left,
                              const int16_t* chroma_left,
                              int16_t above_left_luma,
                              int16_t above_left_chroma,
                              bool has_above, bool has_left) {
    // Collect neighbour pairs. The corner (above-left) is included
    // once, as part of the above row's first sample's neighbour set is
    // not needed -- use the 8 above + 8 left + corner = 17 pairs max.
    // (The corner is a real neighbour pair, so include it.)
    int64_t n = 0;
    int64_t sum_l = 0, sum_c = 0, sum_ll = 0, sum_lc = 0;
    auto add_pair = [&](int16_t l, int16_t c) {
        n += 1;
        sum_l += l;
        sum_c += c;
        sum_ll += static_cast<int64_t>(l) * l;
        sum_lc += static_cast<int64_t>(l) * c;
    };
    if (has_above) {
        for (int i = 0; i < 8; ++i) add_pair(luma_above[i], chroma_above[i]);
    }
    if (has_left) {
        for (int i = 0; i < 8; ++i) add_pair(luma_left[i], chroma_left[i]);
    }
    if (has_above && has_left) {
        add_pair(above_left_luma, above_left_chroma);
    }
    CclmParams params;
    if (n == 0) {
        // No neighbours at all (top-left corner block): slope 0 and
        // offset = mid chroma (128 in Q8), i.e. predict flat mid-grey.
        params.a = 0;
        params.b = 128 * 256;
        return params;
    }
    // Least squares: a = cov/var, b = mean_c - a*mean_l.
    // var = n*sum_ll - sum_l^2, cov = n*sum_lc - sum_l*sum_c.
    const int64_t var = n * sum_ll - sum_l * sum_l;
    const int64_t cov = n * sum_lc - sum_l * sum_c;
    int64_t a_q8 = 0;
    if (var > 0) {
        // a in Q8 = cov*256/var, rounded to nearest.
        a_q8 = (cov * 256 + (var / 2)) / var;
        if (cov < 0) a_q8 = (cov * 256 - (var / 2)) / var;
        if (a_q8 > kMaxSlopeQ8) a_q8 = kMaxSlopeQ8;
        if (a_q8 < -kMaxSlopeQ8) a_q8 = -kMaxSlopeQ8;
    }
    // b in Q8 = (sum_c*256 - a*sum_l) / n, rounded to nearest.
    int64_t b_q8 = (sum_c * 256 - a_q8 * sum_l);
    if (b_q8 >= 0) b_q8 = (b_q8 + n / 2) / n;
    else b_q8 = (b_q8 - n / 2) / n;
    params.a = static_cast<int32_t>(a_q8);
    params.b = static_cast<int32_t>(b_q8);
    return params;
}

void predict_cclm(const int16_t* luma_recon, const CclmParams& params,
                  int16_t* pred, int16_t max_val) {
    for (int i = 0; i < 64; ++i) {
        // (a*luma + b) >> 8, with rounding: add 128 before shifting.
        int32_t v = (params.a * static_cast<int32_t>(luma_recon[i]) +
                     params.b + 128) >> 8;
        pred[i] = static_cast<int16_t>(std::clamp(v, 0, static_cast<int>(max_val)));
    }
}

void downsample_luma_16x16_to_8x8(const int16_t* luma_16, int16_t* luma_8) {
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            const int32_t s = static_cast<int32_t>(luma_16[(2 * r) * 16 + 2 * c]) +
                              static_cast<int32_t>(luma_16[(2 * r) * 16 + 2 * c + 1]) +
                              static_cast<int32_t>(luma_16[(2 * r + 1) * 16 + 2 * c]) +
                              static_cast<int32_t>(luma_16[(2 * r + 1) * 16 + 2 * c + 1]);
            // Round to nearest: (sum + 2) / 4.
            luma_8[r * 8 + c] = static_cast<int16_t>((s + 2) >> 2);
        }
    }
}

void predict_chroma_block_cclm(const int16_t* luma_plane, uint32_t tw,
                               uint32_t th, const int16_t* chroma_plane,
                               uint32_t chroma_tw, uint32_t chroma_th,
                               uint32_t bx, uint32_t by,
                               ChromaSubsampling subsampling, int16_t* pred,
                               int16_t max_val) {
    const uint32_t bx0 = bx * 8;
    const uint32_t by0 = by * 8;
    const bool has_above = by > 0;
    const bool has_left = bx > 0;
    const bool subsampled = subsampling == ChromaSubsampling::YUV420;

    // Co-located reconstructed luma 8x8.
    int16_t luma_8[64];
    if (!subsampled) {
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c) {
                const uint32_t px = std::min(bx0 + c, tw - 1);
                const uint32_t py = std::min(by0 + r, th - 1);
                luma_8[r * 8 + c] = luma_plane[py * tw + px];
            }
    } else {
        // 16x16 luma region at (2*bx0, 2*by0), downsampled to 8x8.
        int16_t luma_16[256];
        for (int r = 0; r < 16; ++r)
            for (int c = 0; c < 16; ++c) {
                const uint32_t px = std::min(2 * bx0 + c, tw - 1);
                const uint32_t py = std::min(2 * by0 + r, th - 1);
                luma_16[r * 16 + c] = luma_plane[py * tw + px];
            }
        downsample_luma_16x16_to_8x8(luma_16, luma_8);
    }

    // Neighbour luma/chroma above and left, downsampled for YUV420.
    int16_t luma_above[8] = {}, luma_left[8] = {};
    int16_t chroma_above[8] = {}, chroma_left[8] = {};
    int16_t above_left_luma = 0, above_left_chroma = 0;
    if (has_above) {
        for (int c = 0; c < 8; ++c) {
            const uint32_t px = std::min(bx0 + c, chroma_tw - 1);
            chroma_above[c] = chroma_plane[(by0 - 1) * chroma_tw + px];
            if (!subsampled) {
                luma_above[c] = luma_plane[(by0 - 1) * tw + std::min(bx0 + c, tw - 1)];
            } else {
                // Average the two luma rows above the chroma row.
                const uint32_t lpx = std::min(2 * bx0 + 2 * c, tw - 1);
                const uint32_t lpx1 = std::min(2 * bx0 + 2 * c + 1, tw - 1);
                const uint32_t lpy0 = 2 * by0 - 2 < th ? 2 * by0 - 2 : th - 1;
                const uint32_t lpy1 = 2 * by0 - 1 < th ? 2 * by0 - 1 : th - 1;
                const int32_t s = static_cast<int32_t>(luma_plane[lpy0 * tw + lpx]) +
                                  static_cast<int32_t>(luma_plane[lpy0 * tw + lpx1]) +
                                  static_cast<int32_t>(luma_plane[lpy1 * tw + lpx]) +
                                  static_cast<int32_t>(luma_plane[lpy1 * tw + lpx1]);
                luma_above[c] = static_cast<int16_t>((s + 2) >> 2);
            }
        }
    }
    if (has_left) {
        for (int r = 0; r < 8; ++r) {
            const uint32_t py = std::min(by0 + r, chroma_th - 1);
            chroma_left[r] = chroma_plane[py * chroma_tw + (bx0 - 1)];
            if (!subsampled) {
                luma_left[r] = luma_plane[py * tw + (bx0 - 1)];
            } else {
                // Average the two luma cols left of the chroma col.
                const uint32_t lpy = std::min(2 * by0 + 2 * r, th - 1);
                const uint32_t lpy1 = std::min(2 * by0 + 2 * r + 1, th - 1);
                const uint32_t lpx0 = 2 * bx0 - 2 < tw ? 2 * bx0 - 2 : tw - 1;
                const uint32_t lpx1 = 2 * bx0 - 1 < tw ? 2 * bx0 - 1 : tw - 1;
                const int32_t s = static_cast<int32_t>(luma_plane[lpy * tw + lpx0]) +
                                  static_cast<int32_t>(luma_plane[lpy * tw + lpx1]) +
                                  static_cast<int32_t>(luma_plane[lpy1 * tw + lpx0]) +
                                  static_cast<int32_t>(luma_plane[lpy1 * tw + lpx1]);
                luma_left[r] = static_cast<int16_t>((s + 2) >> 2);
            }
        }
    }
    if (has_above && has_left) {
        above_left_chroma = chroma_plane[(by0 - 1) * chroma_tw + (bx0 - 1)];
        if (!subsampled) {
            above_left_luma = luma_plane[(by0 - 1) * tw + (bx0 - 1)];
        } else {
            const uint32_t lpx0 = 2 * bx0 - 2 < tw ? 2 * bx0 - 2 : tw - 1;
            const uint32_t lpx1 = 2 * bx0 - 1 < tw ? 2 * bx0 - 1 : tw - 1;
            const uint32_t lpy0 = 2 * by0 - 2 < th ? 2 * by0 - 2 : th - 1;
            const uint32_t lpy1 = 2 * by0 - 1 < th ? 2 * by0 - 1 : th - 1;
            const int32_t s = static_cast<int32_t>(luma_plane[lpy0 * tw + lpx0]) +
                              static_cast<int32_t>(luma_plane[lpy0 * tw + lpx1]) +
                              static_cast<int32_t>(luma_plane[lpy1 * tw + lpx0]) +
                              static_cast<int32_t>(luma_plane[lpy1 * tw + lpx1]);
            above_left_luma = static_cast<int16_t>((s + 2) >> 2);
        }
    }

    const CclmParams params = derive_cclm_params(
        has_above ? luma_above : nullptr, has_above ? chroma_above : nullptr,
        has_left ? luma_left : nullptr, has_left ? chroma_left : nullptr,
        above_left_luma, above_left_chroma, has_above, has_left);
    predict_cclm(luma_8, params, pred, max_val);
}

}  // namespace wk
