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

// HDR slice 1 tests (2026-11-17). The scene generators are the slice-0
// diagnostic's, copied verbatim so the numbers below are comparable
// with tools/hdr_slice0_research_2026-11-17.md (pipeline.csv) and the
// audit (H3/H4). All content is synthetic; there is no native HDR
// corpus yet, and no test here claims anything about real HDR content.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "../src/core/adaptive_chroma.h"
#include "../src/core/cclm.h"
#include "../src/core/colorspace.h"
#include "../src/core/container.h"
#include "../src/core/control_field.h"
#include "../src/core/image_io.h"
#include "../src/core/metrics.h"
#include "../src/core/quantize.h"

using namespace wk;

namespace {

uint16_t* px16(Image& img) {
    return reinterpret_cast<uint16_t*>(img.pixels().data());
}
const uint16_t* px16(const Image& img) {
    return reinterpret_cast<const uint16_t*>(img.pixels().data());
}

// Deterministic LCG texture, full-range at the given bit depth.
// (Same generator as the audit's make_gradient_texture.)
Image make_gradient_texture(uint32_t w, uint32_t h, BitDepth bd) {
    Image img(w, h, bd, false);
    const uint16_t maxv = static_cast<uint16_t>((1u << static_cast<unsigned>(bd)) - 1u);
    auto px = px16(img);
    uint32_t state = 12345;
    auto rnd = [&]() {
        state = state * 1664525u + 1013904223u;
        return (state >> 16) & 0x7fff;
    };
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            uint16_t r = static_cast<uint16_t>((static_cast<uint32_t>(x) * maxv) / (w - 1));
            uint16_t g = static_cast<uint16_t>((static_cast<uint32_t>(y) * maxv) / (h - 1));
            uint16_t b = static_cast<uint16_t>(maxv / 2);
            if (x >= w / 2) {
                const uint16_t chk = ((x / 2 + y / 2) % 2 == 0) ? maxv : 0;
                const int noise = static_cast<int>(rnd() % 64) - 32;
                const int scale = (bd == BitDepth::Bits12) ? 16 : 4;
                r = static_cast<uint16_t>(std::clamp(static_cast<int>(chk) + noise * scale, 0, static_cast<int>(maxv)));
                g = static_cast<uint16_t>(std::clamp(static_cast<int>(b) + noise * scale, 0, static_cast<int>(maxv)));
                b = static_cast<uint16_t>(std::clamp(static_cast<int>(g) + (noise / 2) * scale, 0, static_cast<int>(maxv)));
            }
            px[i + 0] = r;
            px[i + 1] = g;
            px[i + 2] = b;
        }
    }
    return img;
}

Image scale_down_4x(const Image& img10) {
    Image img8(img10.width(), img10.height(), BitDepth::Bits8, false);
    auto s = px16(img10);
    auto d = img8.pixels().data();
    const size_t n = static_cast<size_t>(img10.width()) * img10.height() * 3;
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(s[i] / 4);
    return img8;
}

Image make_ramp_scene() {
    const uint32_t w = 1024, h = 256;
    Image img(w, h, BitDepth::Bits10, false);
    auto px = px16(img);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t v;
            if (y < 128) {
                v = static_cast<uint16_t>(x);  // 0..1023 full ramp
            } else {
                v = static_cast<uint16_t>(x / 8);  // 0..127 shadow ramp
            }
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            px[i + 0] = v;
            px[i + 1] = v;
            px[i + 2] = v;
        }
    }
    return img;
}

struct Planes {
    std::vector<int16_t> y, cb, cr;
};

Image rgb_from_ycbcr(const Planes& p, uint32_t w, uint32_t h, uint8_t matrix,
                     BitDepth bd) {
    Image img(w, h, bd, false);
    ycbcr_to_rgb(img.pixels().data(), p.y.data(), p.cb.data(), p.cr.data(),
                 w, h, matrix, static_cast<uint8_t>(bd), true, false, nullptr);
    return img;
}

Image make_skin_scene() {
    const uint32_t w = 256, h = 256;
    Planes p;
    p.y.assign(static_cast<size_t>(w) * h, 0);
    p.cb.assign(static_cast<size_t>(w) * h, 0);
    p.cr.assign(static_cast<size_t>(w) * h, 0);
    // 4x4 grid of flat skin patches: Y in {256,384,512,640} rows,
    // Cb/Cr in {(440,600),(420,580),(460,620),(440,560)} cols, with a
    // +-4 chroma dither so the uniformity gate passes (as in H7).
    for (uint32_t by = 0; by < 4; ++by) {
        for (uint32_t bx = 0; bx < 4; ++bx) {
            const int16_t yv = static_cast<int16_t>(256 + 128 * static_cast<int>(by));
            const int16_t cbv = static_cast<int16_t>(420 + 20 * static_cast<int>(bx));
            const int16_t crv = static_cast<int16_t>(560 + 20 * static_cast<int>(3 - static_cast<int>(bx)));
            for (uint32_t y = by * 64; y < (by + 1) * 64; ++y) {
                for (uint32_t x = bx * 64; x < (bx + 1) * 64; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    p.y[i] = static_cast<int16_t>(yv + ((x + 2 * y) % 3));
                    p.cb[i] = static_cast<int16_t>(cbv + ((x + 2 * y) % 9) - 4);
                    p.cr[i] = static_cast<int16_t>(crv + ((2 * x + y) % 9) - 4);
                }
            }
        }
    }
    // Bottom strip (y>=224): skin-tone Y ramp 200..800 at Cb=440, Cr=600.
    for (uint32_t y = 224; y < 256; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            p.y[i] = static_cast<int16_t>(200 + (600 * static_cast<int>(x)) / 255);
            p.cb[i] = 440;
            p.cr[i] = 600;
        }
    }
    return rgb_from_ycbcr(p, w, h, 9, BitDepth::Bits10);
}

Image make_wcg_scene() {
    const uint32_t w = 256, h = 256;
    Image img(w, h, BitDepth::Bits10, false);
    auto px = px16(img);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t v = static_cast<uint16_t>((static_cast<uint32_t>(x) * 1023) / 255);
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            if (y < 85) {  // red ramp
                px[i + 0] = v;
                px[i + 1] = 0;
                px[i + 2] = 0;
            } else if (y < 170) {  // green ramp
                px[i + 0] = 0;
                px[i + 1] = v;
                px[i + 2] = 0;
            } else {  // blue ramp
                px[i + 0] = 0;
                px[i + 1] = 0;
                px[i + 2] = v;
            }
        }
    }
    return img;
}

Image make_edge_scene() {
    const uint32_t w = 256, h = 128;
    Image img(w, h, BitDepth::Bits10, false);
    auto px = px16(img);
    const uint16_t bands[8] = {0, 64, 128, 256, 512, 768, 896, 1023};
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t v = bands[x / 32];
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            px[i + 0] = v;
            px[i + 1] = v;
            px[i + 2] = v;
        }
    }
    return img;
}

// Encode + decode + compare in one call; fails the test on any error.
struct Roundtrip {
    size_t size = 0;
    double psnr = -1.0;
    double ssim = -1.0;
};

Roundtrip roundtrip(const Image& img, float quality, BitDepth bd, CICP cicp,
                    bool adaptive_chroma = true) {
    EncoderConfig cfg;
    cfg.quality = quality;
    cfg.bit_depth = bd;
    cfg.cicp = cicp;
    cfg.adaptive_chroma_subsampling = adaptive_chroma;
    auto enc = encode(img, cfg);
    EXPECT_TRUE(enc.has_value()) << (enc ? "" : enc.error().message);
    if (!enc) return {};
    auto dec = decode(*enc);
    EXPECT_TRUE(dec.has_value()) << (dec ? "" : dec.error().message);
    if (!dec) return {enc->size(), -1.0, -1.0};
    EXPECT_EQ(dec->bit_depth(), bd);
    auto m = metrics::compare_images(img, *dec);
    EXPECT_TRUE(m.has_value()) << (m ? "" : m.error().message);
    if (!m) return {enc->size(), -1.0, -1.0};
    return {enc->size(), m->psnr, m->ssim};
}

std::filesystem::path write_temp(const std::string& name,
                                 const std::vector<uint8_t>& bytes) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    return path;
}

void remove_temp(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Builds a P6 PPM byte buffer (16-bit raster is big-endian per spec).
std::vector<uint8_t> make_ppm16(uint32_t w, uint32_t h, uint16_t maxval,
                                const std::vector<uint16_t>& samples) {
    std::string header = "P6\n" + std::to_string(w) + " " + std::to_string(h) +
                         "\n" + std::to_string(maxval) + "\n";
    std::vector<uint8_t> out(header.begin(), header.end());
    for (uint16_t v : samples) {
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xff));
    }
    return out;
}

}  // namespace

// H3 repro (audit): 128x128 gradient+texture, BT.2020/PQ, q75, was
// 15,744 B / PSNR 37.00 / SSIM 0.99030 on the unpatched library
// (lambda x1). The slice-1 lambda fix (x16 at 10-bit) deliberately
// moves the operating point to the 8-bit one (slice 0 Section 3.4) —
// measured post-fix: 13,159 B / 36.58 dB / 0.98898, vs the same
// scene at 8-bit 13,467 B / 36.50 dB / 0.98552. So the floors below
// are the post-fix operating point (above the 8-bit point, with
// margin), not the pre-fix lambda-x1 numbers, and the file must
// shrink below the slice-0 size.
TEST(HdrTest, Roundtrip10BitGradientTextureQ75) {
    Image img = make_gradient_texture(128, 128, BitDepth::Bits10);
    const Roundtrip r = roundtrip(img, 75, BitDepth::Bits10, CICP{9, 16, 9, true});
    EXPECT_LT(r.size, 15744u) << "lambda x16 should spend fewer bits than slice 0 (15744 B)";
    EXPECT_GE(r.psnr, 36.4) << "post-fix: 36.58 dB (8-bit point: 36.50 dB)";
    EXPECT_GE(r.ssim, 0.988) << "post-fix: 0.98898 (8-bit point: 0.98552)";
}

// Pipeline A scenes at q75 (pipeline.csv, slice 0). Ramp/skin/edge
// are near-degenerate for RDO (SSD ~ 0), so the lambda fix barely
// moves them; tex and wcg carry the lambda tradeoff. The floors are
// set from the post-fix operating point, which must match or beat
// the 8-bit point at the same q (slice 0 Section 3.4 criterion) — wcg
// post-fix 54.69 dB / 0.99448 vs 8-bit 51.83 dB / 0.98792; tex256
// post-fix 36.57 dB / 0.98909 vs 8-bit 36.58 dB / 0.98599 — not
// from the pre-fix lambda-x1 numbers (ramp 60.66, skin 49.01, wcg
// 56.01, edge 62.54, tex 36.99), which slice 0 itself declared
// miscalibrated (lambda x1 overspends ~15% bits).
TEST(HdrTest, Roundtrip10BitPipelineScenesQ75) {
    const CICP cicp{9, 16, 9, true};
    {
        const Roundtrip r = roundtrip(make_ramp_scene(), 75, BitDepth::Bits10, cicp);
        EXPECT_GE(r.psnr, 60.0) << "slice 0 ramp A: 60.66 dB";
        EXPECT_GE(r.ssim, 0.999) << "slice 0 ramp A: 0.99960";
    }
    {
        const Roundtrip r = roundtrip(make_skin_scene(), 75, BitDepth::Bits10, cicp);
        EXPECT_GE(r.psnr, 48.4) << "slice 0 skin A: 49.01 dB";
        EXPECT_GE(r.ssim, 0.985) << "slice 0 skin A: 0.98653";
    }
    {
        const Roundtrip r = roundtrip(make_wcg_scene(), 75, BitDepth::Bits10, cicp);
        EXPECT_GE(r.psnr, 53.5) << "post-fix: 54.69 dB (8-bit point: 51.83 dB)";
        EXPECT_GE(r.ssim, 0.992) << "post-fix: 0.99448 (8-bit point: 0.98792)";
    }
    {
        const Roundtrip r = roundtrip(make_edge_scene(), 75, BitDepth::Bits10, cicp);
        EXPECT_GE(r.psnr, 62.0) << "slice 0 edge A: 62.54 dB";
        EXPECT_GE(r.ssim, 0.999) << "slice 0 edge A: 0.99920";
    }
    {
        const Roundtrip r = roundtrip(make_gradient_texture(256, 256, BitDepth::Bits10),
                                      75, BitDepth::Bits10, cicp);
        EXPECT_GE(r.psnr, 36.4) << "post-fix: 36.57 dB (8-bit point: 36.58 dB)";
        EXPECT_GE(r.ssim, 0.988) << "post-fix: 0.98909 (8-bit point: 0.98599)";
    }
}

// 12-bit is verify-only in v1: same scene at 12-bit must round-trip
// in the slice-0 band (audit H4 q75: PSNR 36.96, SSIM 0.99054).
TEST(HdrTest, Roundtrip12BitGradientTextureQ75SpotCheck) {
    Image img = make_gradient_texture(128, 128, BitDepth::Bits12);
    const Roundtrip r = roundtrip(img, 75, BitDepth::Bits12, CICP{9, 16, 9, true});
    EXPECT_GE(r.psnr, 36.4) << "audit H4: 36.96 dB";
    EXPECT_GE(r.ssim, 0.989) << "audit H4: 0.99054";
}

// HLG is signalled and round-tripped in v1, not fully measured:
// same content with transfer 18 must behave like PQ (slice 0 Section 2.5:
// identical size/PSNR/SSIM on ramp q75, because pipeline A never
// calls the EOTF).
TEST(HdrTest, Roundtrip10BitHlgMatchesPq) {
    Image img = make_gradient_texture(128, 128, BitDepth::Bits10);
    const Roundtrip pq = roundtrip(img, 75, BitDepth::Bits10, CICP{9, 16, 9, true});
    const Roundtrip hlg = roundtrip(img, 75, BitDepth::Bits10, CICP{9, 18, 9, true});
    EXPECT_EQ(hlg.size, pq.size);
    EXPECT_DOUBLE_EQ(hlg.psnr, pq.psnr);
    EXPECT_DOUBLE_EQ(hlg.ssim, pq.ssim);
}

// Adaptive-chroma oracle (slice 0 Section 5.2): the scaled thresholds
// (8.4/316/400 at 10-bit) must reproduce the 8-bit decision
// (2.1/79/100) on the same content scaled x4.
TEST(HdrTest, AdaptiveChromaScaledThresholdsReproduce8BitDecision) {
    // Smooth chroma: a gentle sinusoid, c_lap in (0.8, 2.0) at 8-bit.
    const uint32_t w = 256, h = 256;
    Image img10(w, h, BitDepth::Bits10, false);
    {
        auto px = px16(img10);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const double s = std::sin(2.0 * M_PI * static_cast<double>(x) / 32.0);
                const int v = static_cast<int>(std::lround(156.0 * s));
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                px[i + 0] = static_cast<uint16_t>(512 + v);
                px[i + 1] = 512;
                px[i + 2] = static_cast<uint16_t>(512 - v);
            }
        }
    }
    Image img8 = scale_down_4x(img10);
    Planes p8, p10;
    const size_t n = static_cast<size_t>(w) * h;
    p8.y.resize(n); p8.cb.resize(n); p8.cr.resize(n);
    p10.y.resize(n); p10.cb.resize(n); p10.cr.resize(n);
    rgb_to_ycbcr(p8.y.data(), p8.cb.data(), p8.cr.data(), img8.pixels().data(),
                 w, h, 9, 8, true, false);
    rgb_to_ycbcr(p10.y.data(), p10.cb.data(), p10.cr.data(), img10.pixels().data(),
                 w, h, 9, 10, true, false);
    const ChromaLaplacianStats stats8 =
        chroma_laplacian_stats(p8.cb.data(), p8.cr.data(), w, h, 0, 0, w, h, 8);
    ASSERT_GE(stats8.mean, 0.8) << "test premise: 8-bit c_lap must sit below 2.1";
    ASSERT_LT(stats8.mean, 2.0) << "test premise: 8-bit c_lap must sit below 2.1";
    const bool dec8 = should_subsample_chroma_420(p8.cb.data(), p8.cr.data(), w, h,
                                                  0, 0, w, h, 2.1, 79.0, 100.0, 8);
    const bool dec10 = should_subsample_chroma_420(p10.cb.data(), p10.cr.data(), w, h,
                                                   0, 0, w, h, 8.4, 316.0, 400.0, 10);
    EXPECT_TRUE(dec8) << "smooth chroma subsamples at 8-bit";
    EXPECT_TRUE(dec10) << "scaled thresholds must agree at 10-bit";
    EXPECT_EQ(dec10, dec8);

    // Sharp step edge: p999 must force 444 at both depths. Step
    // amplitude 136 (8-bit) gives p999 136 >= 79 while the mean stays
    // ~0.5, so only the p999 condition can force 444.
    std::vector<int16_t> cb8(n, 64), cr8(n, 128), cb10(n, 256), cr10(n, 512);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = w / 2; x < w; ++x) {
            cb8[static_cast<size_t>(y) * w + x] = 200;
            cb10[static_cast<size_t>(y) * w + x] = 800;
        }
    }
    const bool edge8 = should_subsample_chroma_420(cb8.data(), cr8.data(), w, h,
                                                   0, 0, w, h, 2.1, 79.0, 100.0, 8);
    const bool edge10 = should_subsample_chroma_420(cb10.data(), cr10.data(), w, h,
                                                    0, 0, w, h, 8.4, 316.0, 400.0, 10);
    EXPECT_FALSE(edge8) << "step edge stays 444 at 8-bit";
    EXPECT_FALSE(edge10) << "scaled thresholds must agree at 10-bit";
    EXPECT_EQ(edge10, edge8);
}

// Encoder-level adaptive check (slice 0 Section 5.2: unscaled thresholds
// waste 171,943 B = 10.9% on the corpus by keeping 444 where the
// oracle says 420). On the smooth-chroma scene above, adaptive ON
// must beat OFF at 10-bit; the 8-bit pair is the control.
TEST(HdrTest, AdaptiveChromaEncoderPicks420At10Bit) {
    const uint32_t w = 256, h = 256;
    Image img10(w, h, BitDepth::Bits10, false);
    {
        auto px = px16(img10);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const double s = std::sin(2.0 * M_PI * static_cast<double>(x) / 32.0);
                const int v = static_cast<int>(std::lround(156.0 * s));
                const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                px[i + 0] = static_cast<uint16_t>(512 + v);
                px[i + 1] = 512;
                px[i + 2] = static_cast<uint16_t>(512 - v);
            }
        }
    }
    const CICP cicp{9, 16, 9, true};
    const Roundtrip on10 = roundtrip(img10, 75, BitDepth::Bits10, cicp, true);
    const Roundtrip off10 = roundtrip(img10, 75, BitDepth::Bits10, cicp, false);
    EXPECT_LT(on10.size, off10.size)
        << "adaptive ON must pick 420 on smooth 10-bit chroma (was 444 before the fix)";
    Image img8 = scale_down_4x(img10);
    const Roundtrip on8 = roundtrip(img8, 75, BitDepth::Bits8, CICP{1, 1, 1, true}, true);
    const Roundtrip off8 = roundtrip(img8, 75, BitDepth::Bits8, CICP{1, 1, 1, true}, false);
    EXPECT_LT(on8.size, off8.size) << "8-bit control: adaptive ON beats OFF";
}

// The encoder must reject a bit-depth mismatch instead of reading
// the pixel buffer at the wrong sample size (rgb_to_ycbcr reads
// uint16 samples at 10-bit: an 8-bit buffer would be read 2x past
// its end).
TEST(HdrTest, EncodeRejectsBitDepthMismatch) {
    Image img(16, 16, BitDepth::Bits8, false);
    std::fill(img.pixels().begin(), img.pixels().end(), 128);
    EncoderConfig cfg;
    cfg.bit_depth = BitDepth::Bits10;
    auto enc = encode(img, cfg);
    EXPECT_FALSE(enc.has_value()) << "8-bit pixels with a 10-bit config must fail";
}

// B6: the encoder must set the HDR flag by the same rule the decoder
// checks (set exactly when bit_depth > 8 or transfer is PQ/HLG), or
// its own output would fail the decoder's agreement check. Before
// this fix the encoder only looked at bit_depth, so an 8-bit PQ file
// came out with the flag clear.
TEST(HdrTest, EncoderSetsHdrFlagFromBitDepthAndTransfer) {
    auto encode_and_read_flag = [](const Image& img, BitDepth bd, CICP cicp) {
        EncoderConfig cfg;
        cfg.quality = 75;
        cfg.bit_depth = bd;
        cfg.cicp = cicp;
        auto enc = encode(img, cfg);
        EXPECT_TRUE(enc.has_value()) << (enc ? "" : enc.error().message);
        if (!enc) return false;
        auto file = parse_container(*enc);
        EXPECT_TRUE(file.has_value()) << (file ? "" : file.error().message);
        if (!file) return false;
        EXPECT_TRUE(decode(*enc).has_value()) << "round-trip must decode";
        return file->header.is_hdr();
    };

    // 8-bit images come from the 10-bit generator via scale_down_4x:
    // make_gradient_texture writes uint16 samples, so it must never be
    // called with Bits8 (that overflows the 3-byte-per-pixel buffer).
    const Image img10 = make_gradient_texture(16, 16, BitDepth::Bits10);
    const Image img8 = scale_down_4x(img10);

    // 8-bit BT.709: flag clear.
    EXPECT_FALSE(encode_and_read_flag(img8, BitDepth::Bits8, CICP{1, 1, 1, true}));
    // 8-bit PQ: flag set.
    EXPECT_TRUE(encode_and_read_flag(img8, BitDepth::Bits8, CICP{9, 16, 9, true}));
    // 8-bit HLG: flag set.
    EXPECT_TRUE(encode_and_read_flag(img8, BitDepth::Bits8, CICP{9, 18, 9, true}));
    // 10-bit PQ: flag set.
    EXPECT_TRUE(encode_and_read_flag(img10, BitDepth::Bits10, CICP{9, 16, 9, true}));
}

// The encoder must reject CICP primaries with no entry in ITU-T
// H.273 Table 2, with the same rule the decoder checks (B5): an
// encoder that writes primaries=99 produces a file its own decoder
// rejects, so the failure must happen at encode time, not at decode.
TEST(HdrTest, EncoderRejectsUnknownPrimaries) {
    const Image img10 = make_gradient_texture(16, 16, BitDepth::Bits10);
    const Image img8 = scale_down_4x(img10);

    for (uint8_t primaries : {99, 255}) {
        EncoderConfig cfg;
        cfg.quality = 75;
        cfg.bit_depth = BitDepth::Bits8;
        cfg.cicp = CICP{primaries, 1, 1, true};
        auto enc = encode(img8, cfg);
        ASSERT_FALSE(enc.has_value()) << "primaries=" << static_cast<int>(primaries);
        EXPECT_EQ(enc.error().code, ErrorCode::InvalidParameter);
    }

    // Every Table 2 value still encodes and round-trips.
    for (uint8_t primaries : {0, 2, 5, 9, 22}) {
        EncoderConfig cfg;
        cfg.quality = 75;
        cfg.bit_depth = BitDepth::Bits8;
        cfg.cicp = CICP{primaries, 1, 1, true};
        auto enc = encode(img8, cfg);
        ASSERT_TRUE(enc.has_value())
            << "primaries=" << static_cast<int>(primaries)
            << (enc ? "" : enc.error().message);
        EXPECT_TRUE(decode(*enc).has_value()) << "round-trip must decode";
    }
}

// Lambda RDO must scale with the square of the code-value range:
// x16 at 10-bit, x256 at 12-bit (slice 0 Section 3.4). 8-bit is untouched.
TEST(HdrTest, LambdaScalesWithBitDepth) {
    const float l8 = qp_to_lambda(25);
    EXPECT_NEAR(l8, 17.135f, 0.01f) << "q75 lambda is unchanged at 8-bit";
    EXPECT_NEAR(qp_to_lambda(25, 10), l8 * 16.0f, 0.01f);
    EXPECT_NEAR(qp_to_lambda(25, 12), l8 * 256.0f, 0.1f);
    EXPECT_NEAR(qp_to_lambda(25, 8), l8, 0.001f);
}

// CCLM B3: the no-neighbour fallback mid is the bit-depth mid
// (512 at 10-bit, 2048 at 12-bit), not the hardcoded 8-bit 128.
TEST(HdrTest, CclmFallbackMidScalesWithBitDepth) {
    const CclmParams p8 = derive_cclm_params(nullptr, nullptr, nullptr, nullptr,
                                            0, 0, false, false, 255);
    EXPECT_EQ(p8.a, 0);
    EXPECT_EQ(p8.b, 128 * 256);
    const CclmParams p10 = derive_cclm_params(nullptr, nullptr, nullptr, nullptr,
                                             0, 0, false, false, 1023);
    EXPECT_EQ(p10.a, 0);
    EXPECT_EQ(p10.b, 512 * 256);
    const CclmParams p12 = derive_cclm_params(nullptr, nullptr, nullptr, nullptr,
                                             0, 0, false, false, 4095);
    EXPECT_EQ(p12.a, 0);
    EXPECT_EQ(p12.b, 2048 * 256);
    // Default is the 8-bit mid, so existing callers are unaffected.
    const CclmParams dflt = derive_cclm_params(nullptr, nullptr, nullptr, nullptr,
                                               0, 0, false, false);
    EXPECT_EQ(dflt.b, 128 * 256);
}

// Control-field equivalence (slice 0 Section 4.3): the recalibrated 10-bit
// field must reproduce the 8-bit classification exactly on the same
// content scaled x4 (100.00% on 176,697 blocks in slice 0).
TEST(HdrTest, ControlField10BitReproduces8Bit) {
    const uint32_t w = 128, h = 128;
    Image img10 = make_gradient_texture(w, h, BitDepth::Bits10);
    Image img8 = scale_down_4x(img10);
    const size_t n = static_cast<size_t>(w) * h;
    Planes p8, p10;
    p8.y.resize(n); p8.cb.resize(n); p8.cr.resize(n);
    rgb_to_ycbcr(p8.y.data(), p8.cb.data(), p8.cr.data(), img8.pixels().data(),
                 w, h, 1, 8, true, false);
    // Exact x4 of the 8-bit planes (the slice-0 premise), not a second
    // RGB->YCbCr pass (whose rounding would differ by a code or two).
    p10.y.resize(n); p10.cb.resize(n); p10.cr.resize(n);
    for (size_t i = 0; i < n; ++i) {
        p10.y[i] = static_cast<int16_t>(p8.y[i] * 4);
        p10.cb[i] = static_cast<int16_t>(p8.cb[i] * 4);
        p10.cr[i] = static_cast<int16_t>(p8.cr[i] * 4);
    }
    const auto f8 = analyze_control_field(p8.y.data(), w, h, p8.cb.data(), p8.cr.data(), 8);
    const auto f10 = analyze_control_field(p10.y.data(), w, h, p10.cb.data(), p10.cr.data(), 10);
    ASSERT_EQ(f8.size(), f10.size());
    size_t agree = 0;
    for (size_t i = 0; i < f8.size(); ++i) {
        if (f8[i].content == f10[i].content &&
            f8[i].quant_scale_index == f10[i].quant_scale_index) {
            ++agree;
        }
        EXPECT_NEAR(f10[i].chroma_grad, f8[i].chroma_grad * 4.0f, 0.5f)
            << "block " << i << ": chroma_grad must scale x4";
    }
    EXPECT_EQ(agree, f8.size()) << "10-bit field must reproduce 8-bit exactly";

    // Skin patch: +-4 chroma dither (as in H7) so the uniformity gate
    // passes (within-block std ~2.6) while the chroma gradient stays
    // ~1.8, below the FP threshold.
    const uint32_t sw = 64, sh = 64;
    const size_t sn = static_cast<size_t>(sw) * sh;
    Planes s8, s10;
    s8.y.assign(sn, 128); s8.cb.assign(sn, 128); s8.cr.assign(sn, 128);
    for (uint32_t y = 16; y < 48; ++y) {
        for (uint32_t x = 16; x < 48; ++x) {
            const size_t i = static_cast<size_t>(y) * sw + x;
            s8.y[i] = 140;
            s8.cb[i] = static_cast<int16_t>(110 + ((x + 2 * y) % 9) - 4);
            s8.cr[i] = static_cast<int16_t>(150 + ((2 * x + y) % 9) - 4);
        }
    }
    s10.y.resize(sn); s10.cb.resize(sn); s10.cr.resize(sn);
    for (size_t i = 0; i < sn; ++i) {
        s10.y[i] = static_cast<int16_t>(s8.y[i] * 4);
        s10.cb[i] = static_cast<int16_t>(s8.cb[i] * 4);
        s10.cr[i] = static_cast<int16_t>(s8.cr[i] * 4);
    }
    const auto sf8 = analyze_control_field(s8.y.data(), sw, sh, s8.cb.data(), s8.cr.data(), 8);
    const auto sf10 = analyze_control_field(s10.y.data(), sw, sh, s10.cb.data(), s10.cr.data(), 10);
    ASSERT_EQ(sf8.size(), sf10.size());
    size_t skin8 = 0, skin10 = 0, skin_agree = 0;
    for (size_t i = 0; i < sf8.size(); ++i) {
        if (sf8[i].content == BlockContent::Skin) ++skin8;
        if (sf10[i].content == BlockContent::Skin) ++skin10;
        if (sf8[i].content == sf10[i].content) ++skin_agree;
    }
    EXPECT_GT(skin8, 0u) << "patch must classify Skin at 8-bit";
    EXPECT_EQ(skin10, skin8) << "Skin recall must be 100% at 10-bit (slice 0: 2179/2179)";
    EXPECT_EQ(skin_agree, sf8.size());
    const auto m8 = skin_false_positive_mask(sf8, (sw + 7) / 8, (sh + 7) / 8, 8);
    const auto m10 = skin_false_positive_mask(sf10, (sw + 7) / 8, (sh + 7) / 8, 10);
    EXPECT_EQ(m10, m8) << "FP mask must agree (chroma-grad threshold 3.0 -> 12.0)";
    EXPECT_EQ(std::count(m8.begin(), m8.end(), 1), 0)
        << "smooth skin patch must not be dropped at either depth";
}

// 16-bit PPM input: maxval 1023 loads as 10-bit, maxval 4095 as
// 12-bit, other maxvals and truncated payloads are rejected.
TEST(HdrTest, Ppm16BitLoad) {
    const std::vector<uint16_t> samples = {0, 512, 1023, 256, 768, 100,
                                           1023, 0, 512, 100, 200, 300};
    auto path = write_temp("wk_hdr10.ppm", make_ppm16(2, 2, 1023, samples));
    auto loaded = io::load_image_file(path.string());
    remove_temp(path);
    ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    EXPECT_EQ(loaded->width(), 2u);
    EXPECT_EQ(loaded->height(), 2u);
    const uint16_t* px = reinterpret_cast<const uint16_t*>(loaded->pixels().data());
    for (size_t i = 0; i < samples.size(); ++i) {
        EXPECT_EQ(px[i], samples[i]) << "sample " << i;
    }

    auto path12 = write_temp("wk_hdr12.ppm", make_ppm16(2, 2, 4095, samples));
    auto loaded12 = io::load_image_file(path12.string());
    remove_temp(path12);
    ASSERT_TRUE(loaded12.has_value());
    EXPECT_EQ(loaded12->bit_depth(), BitDepth::Bits12);

    auto bad = write_temp("wk_hdr_bad.ppm", make_ppm16(2, 2, 100, samples));
    EXPECT_FALSE(io::load_image_file(bad.string()).has_value());
    remove_temp(bad);

    std::vector<uint8_t> truncated = make_ppm16(2, 2, 1023, samples);
    truncated.resize(truncated.size() - 4);  // drop 2 samples
    auto trunc = write_temp("wk_hdr_trunc.ppm", truncated);
    EXPECT_FALSE(io::load_image_file(trunc.string()).has_value());
    remove_temp(trunc);
}

// 16-bit PPM output: save_ppm must write maxval 1023/4095 with a
// big-endian raster, and a save->load round-trip must be exact.
TEST(HdrTest, Ppm16BitSaveRoundtrip) {
    Image img = make_gradient_texture(16, 16, BitDepth::Bits10);
    auto path = std::filesystem::temp_directory_path() / "wk_hdr10_out.ppm";
    auto saved = io::save_image_file(path.string(), img);
    ASSERT_TRUE(saved.has_value()) << (saved ? "" : saved.error().message);
    auto loaded = io::load_image_file(path.string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->bit_depth(), BitDepth::Bits10);
    EXPECT_EQ(loaded->pixels().size(), img.pixels().size());
    EXPECT_TRUE(std::equal(loaded->pixels().begin(), loaded->pixels().end(),
                           img.pixels().begin()));
    remove_temp(path);

    Image img12 = make_gradient_texture(16, 16, BitDepth::Bits12);
    auto path12 = std::filesystem::temp_directory_path() / "wk_hdr12_out.ppm";
    ASSERT_TRUE(io::save_image_file(path12.string(), img12).has_value());
    auto loaded12 = io::load_image_file(path12.string());
    ASSERT_TRUE(loaded12.has_value());
    EXPECT_EQ(loaded12->bit_depth(), BitDepth::Bits12);
    EXPECT_TRUE(std::equal(loaded12->pixels().begin(), loaded12->pixels().end(),
                           img12.pixels().begin()));
    remove_temp(path12);
}
