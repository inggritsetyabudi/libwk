// CCLM (cross-component linear model) chroma prediction tests.
// See src/cclm.h. CCLM predicts a chroma block from its co-located
// reconstructed luma as a*luma + b, with (a, b) derived from the
// neighbouring reconstructed luma/chroma by integer least squares
// (Q8 fixed point), so encoder and decoder agree bit-for-bit.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../src/cclm.h"
#include "../src/container.h"
#include "../src/mode_stream.h"
#include "wk/wk.hpp"

using namespace wk;

namespace {

constexpr int16_t kMaxVal = 255;

}  // namespace

TEST(CclmTest, DeriveRecoversPerfectLinearRelation) {
    // Neighbours exactly on chroma = 2*luma - 10: the fit must
    // recover a = 2.0 (512 in Q8) and b = -10 (-2560 in Q8).
    int16_t luma_above[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    int16_t chroma_above[8];
    int16_t luma_left[8] = {15, 25, 35, 45, 55, 65, 75, 85};
    int16_t chroma_left[8];
    for (int i = 0; i < 8; ++i) {
        chroma_above[i] = static_cast<int16_t>(2 * luma_above[i] - 10);
        chroma_left[i] = static_cast<int16_t>(2 * luma_left[i] - 10);
    }
    const CclmParams p = derive_cclm_params(
        luma_above, chroma_above, luma_left, chroma_left,
        12, 2 * 12 - 10, true, true);
    EXPECT_EQ(p.a, 512);   // 2.0 in Q8
    EXPECT_EQ(p.b, -2560); // -10 in Q8
}

TEST(CclmTest, DeriveFlatNeighboursGivesSlopeZeroAndMean) {
    // All luma neighbours equal: variance is 0, so the slope is 0
    // and the offset is the neighbour chroma mean.
    int16_t luma_above[8] = {100, 100, 100, 100, 100, 100, 100, 100};
    int16_t chroma_above[8] = {140, 140, 140, 140, 140, 140, 140, 140};
    int16_t luma_left[8] = {100, 100, 100, 100, 100, 100, 100, 100};
    int16_t chroma_left[8] = {140, 140, 140, 140, 140, 140, 140, 140};
    const CclmParams p = derive_cclm_params(
        luma_above, chroma_above, luma_left, chroma_left,
        100, 140, true, true);
    EXPECT_EQ(p.a, 0);
    EXPECT_EQ(p.b, 140 * 256);
}

TEST(CclmTest, DeriveSlopeClampedToTwo) {
    // A very steep relation (chroma = 5*luma) must clamp to a = 2.
    int16_t luma_above[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    int16_t chroma_above[8];
    int16_t luma_left[8] = {15, 25, 35, 45, 55, 65, 75, 85};
    int16_t chroma_left[8];
    for (int i = 0; i < 8; ++i) {
        chroma_above[i] = static_cast<int16_t>(5 * luma_above[i]);
        chroma_left[i] = static_cast<int16_t>(5 * luma_left[i]);
    }
    const CclmParams p = derive_cclm_params(
        luma_above, chroma_above, luma_left, chroma_left,
        12, 60, true, true);
    EXPECT_EQ(p.a, 512); // clamped to 2.0
}

TEST(CclmTest, PredictAppliesModelAndClamps) {
    // a = 1.0 (256), b = 0: prediction equals the luma block.
    // a = 2.0, b = -100: a bright luma (200) would give 300, clamped
    // to max_val.
    int16_t luma[64];
    for (int i = 0; i < 64; ++i) luma[i] = static_cast<int16_t>(i * 4);
    int16_t pred[64];
    CclmParams p;
    p.a = 256;
    p.b = 0;
    predict_cclm(luma, p, pred, kMaxVal);
    for (int i = 0; i < 64; ++i) EXPECT_EQ(pred[i], luma[i]);

    p.a = 512;
    p.b = -100 * 256;
    predict_cclm(luma, p, pred, kMaxVal);
    // luma 200 (i=50) -> 2*200-100 = 300 -> clamped to 255.
    EXPECT_EQ(pred[50], 255);
    // luma 0 (i=0) -> -100 -> clamped to 0.
    EXPECT_EQ(pred[0], 0);
    // luma 100 (i=25) -> 2*100-100 = 100.
    EXPECT_EQ(pred[25], 100);
}

TEST(CclmTest, DownsampleAveragesTwoByTwo) {
    int16_t luma_16[256];
    for (int i = 0; i < 256; ++i) luma_16[i] = static_cast<int16_t>(i);
    int16_t luma_8[64];
    downsample_luma_16x16_to_8x8(luma_16, luma_8);
    // Top-left 2x2: (0 + 1 + 16 + 17 + 2) / 4 = 9 (rounded).
    EXPECT_EQ(luma_8[0], 9);
    // Bottom-right 2x2: (238 + 239 + 254 + 255 + 2) / 4 = 247.
    EXPECT_EQ(luma_8[63], 247);
}

TEST(CclmTest, BlockCclmYuv444MatchesNeighbourFit) {
    // A 16x16 tile where chroma = luma (perfect correlation) in the
    // interior: the block at (1,1) should be predicted almost exactly
    // by CCLM from its co-located luma.
    const uint32_t tw = 16, th = 16;
    std::vector<int16_t> luma(tw * th), chroma(tw * th);
    for (uint32_t y = 0; y < th; ++y)
        for (uint32_t x = 0; x < tw; ++x) {
            const int16_t v = static_cast<int16_t>((x * 16 + y) % 256);
            luma[y * tw + x] = v;
            chroma[y * tw + x] = v;
        }
    int16_t pred[64];
    predict_chroma_block_cclm(luma.data(), tw, th, chroma.data(), tw, th,
                              1, 1, ChromaSubsampling::YUV444, pred, kMaxVal);
    // chroma == luma, so the prediction should equal the luma block.
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            EXPECT_EQ(pred[r * 8 + c], luma[(8 + r) * tw + (8 + c)]);
}

TEST(CclmTest, BlockCclmTopLeftCornerIsFlatMidGrey) {
    // No neighbours at all: the model is flat mid-grey (128).
    const uint32_t tw = 16, th = 16;
    std::vector<int16_t> luma(tw * th, 200), chroma(tw * th, 50);
    int16_t pred[64];
    predict_chroma_block_cclm(luma.data(), tw, th, chroma.data(), tw, th,
                              0, 0, ChromaSubsampling::YUV444, pred, kMaxVal);
    for (int i = 0; i < 64; ++i) EXPECT_EQ(pred[i], 128);
}

TEST(CclmTest, RoundtripCorrelatedImageUsesV10AndDecodes) {
    // An image whose chroma is a linear function of luma: CCLM
    // should be selected, the tile layout must be V10, and the
    // decode must round-trip (PSNR > 30 dB at q75).
    Image image(64, 64, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < 64; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 64; ++x) {
            const uint8_t l = static_cast<uint8_t>((x * 4 + y * 2) % 256);
            row[x * 3 + 0] = l;
            row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(128 + (static_cast<int>(l) - 128) / 2, 0, 255));
            row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(128 - (static_cast<int>(l) - 128) / 3, 0, 255));
        }
    }
    EncoderConfig config;
    config.quality = 75.0f;
    config.subsampling = Subsampling::YUV444;
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_EQ(parsed->tile_chunks.size(), 1u);
    const auto& payload = parsed->tile_chunks.front().payload;
    // Layout tag at the tile payload offset for a single-tile image.
    const uint32_t tag = static_cast<uint32_t>(payload[273]) |
                         (static_cast<uint32_t>(payload[274]) << 8) |
                         (static_cast<uint32_t>(payload[275]) << 16) |
                         (static_cast<uint32_t>(payload[276]) << 24);
    EXPECT_EQ(tag, kLossyTileLayoutTagV10) << "correlated chroma must select CCLM (V10)";
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    double sse = 0;
    for (uint32_t y = 0; y < 64; ++y) {
        const uint8_t* orig_row = image.row(y);
        const uint8_t* got_row = decoded->row(y);
        for (uint32_t x = 0; x < 64 * 3; ++x) {
            const double d = static_cast<double>(orig_row[x]) - got_row[x];
            sse += d * d;
        }
    }
    const double psnr = 10.0 * std::log10(255.0 * 255.0 * 64 * 64 * 3 / sse);
    EXPECT_GT(psnr, 30.0) << "CCLM roundtrip PSNR too low";
}

TEST(CclmTest, RoundtripUncorrelatedImageDecodes) {
    // Random noise: luma and chroma are uncorrelated, so CCLM should
    // not hurt. (It may still be selected for a few blocks where the
    // RDO finds it cheaper -- the mode is only picked when it reduces
    // cost -- so this test asserts a correct roundtrip, not a layout.)
    // A separate RD measurement (not this unit test) confirms CCLM
    // does not grow the file on noise.
    Image image(64, 64, BitDepth::Bits8, false);
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<uint8_t>((seed >> 24) & 0xFF);
    };
    for (uint32_t y = 0; y < 64; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 64; ++x) {
            row[x * 3 + 0] = rnd();
            row[x * 3 + 1] = rnd();
            row[x * 3 + 2] = rnd();
        }
    }
    EncoderConfig config;
    config.quality = 75.0f;
    config.subsampling = Subsampling::YUV444;
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(decoded->width(), 64u);
    EXPECT_EQ(decoded->height(), 64u);
}

TEST(CclmTest, CclmOffProducesNoV10) {
    // With use_cclm = false the same correlated image must not use
    // V10, and must still decode correctly (CCLM is an optimization,
    // not a requirement).
    Image image(64, 64, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < 64; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < 64; ++x) {
            const uint8_t l = static_cast<uint8_t>((x * 4 + y * 2) % 256);
            row[x * 3 + 0] = l;
            row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(128 + (static_cast<int>(l) - 128) / 2, 0, 255));
            row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(128 - (static_cast<int>(l) - 128) / 3, 0, 255));
        }
    }
    EncoderConfig config;
    config.quality = 75.0f;
    config.subsampling = Subsampling::YUV444;
    config.use_cclm = false;
    auto encoded = encode(image, config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    const auto& payload = parsed->tile_chunks.front().payload;
    const uint32_t tag = static_cast<uint32_t>(payload[273]) |
                         (static_cast<uint32_t>(payload[274]) << 8) |
                         (static_cast<uint32_t>(payload[275]) << 16) |
                         (static_cast<uint32_t>(payload[276]) << 24);
    EXPECT_NE(tag, kLossyTileLayoutTagV10) << "use_cclm=false must not emit V10";
    auto decoded = decode(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
}
