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

// Fase 9 Slice 3 — encoder animasi: round-trip encode_animation ->
// decode_frame per frame (Slice 2), compositing OVER/SOURCE + disposal
// pada pola rect tumpang-tindih, dan penolakan input buruk.
// Lossy frames are only ever compared against the decode of the same
// frame encoded as a still (off by 1 is normal lossy error); lossless
// frames are compared pixel-exact. #anim #fase9

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>
#include <wk/wk.hpp>

#include "../src/core/common.h"
#include "../src/core/container.h"

using namespace wk;

namespace {

Image make_solid_rgb(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    Image image(width, height, BitDepth::Bits8, false);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
    }
    return image;
}

Image make_solid_rgba(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Image image(width, height, BitDepth::Bits8, true);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = image.row(y);
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = a;
        }
    }
    return image;
}

// Pixel (r,g,b,a) of an 8-bit decoded image; a=255 when the image has no alpha.
std::array<uint8_t, 4> pixel_rgba(const Image& image, uint32_t x, uint32_t y) {
    const uint8_t* p = image.row(y) + static_cast<size_t>(x) * image.bytes_per_pixel();
    if (image.has_alpha()) return {p[0], p[1], p[2], p[3]};
    return {p[0], p[1], p[2], 255};
}

void expect_pixel(const Image& image, uint32_t x, uint32_t y,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    const auto got = pixel_rgba(image, x, y);
    EXPECT_EQ(got[0], r) << "R at (" << x << "," << y << ")";
    EXPECT_EQ(got[1], g) << "G at (" << x << "," << y << ")";
    EXPECT_EQ(got[2], b) << "B at (" << x << "," << y << ")";
    EXPECT_EQ(got[3], a) << "A at (" << x << "," << y << ")";
}

void expect_region(const Image& image, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    for (uint32_t y = y0; y < y0 + h; ++y) {
        for (uint32_t x = x0; x < x0 + w; ++x) {
            expect_pixel(image, x, y, r, g, b, a);
        }
    }
}

void expect_images_equal(const Image& got, const Image& want) {
    ASSERT_EQ(got.width(), want.width());
    ASSERT_EQ(got.height(), want.height());
    ASSERT_EQ(got.bit_depth(), want.bit_depth());
    ASSERT_EQ(got.has_alpha(), want.has_alpha());
    ASSERT_EQ(got.pixels().size(), want.pixels().size());
    EXPECT_EQ(std::memcmp(got.pixels().data(), want.pixels().data(), got.pixels().size()), 0);
}

AnimFrameInput frame(Image image, uint16_t delay_ms = 100, uint8_t blend_mode = 0,
                     uint8_t disposal = 0, uint16_t rect_x = 0, uint16_t rect_y = 0) {
    AnimFrameInput input;
    input.image = std::move(image);
    input.delay_ms = delay_ms;
    input.blend_mode = blend_mode;
    input.disposal = disposal;
    input.rect_x = rect_x;
    input.rect_y = rect_y;
    return input;
}

EncoderConfig anim_config(std::vector<AnimFrameInput> frames, uint16_t loop_count = 0,
                          uint32_t background_rgba = 0x00000000u, bool lossless = false) {
    EncoderConfig config;
    config.lossless = lossless;
    config.tile_size_log2 = 6;
    config.animation.frames = std::move(frames);
    config.animation.loop_count = loop_count;
    config.animation.background_rgba = background_rgba;
    return config;
}

}  // namespace

TEST(AnimationEncoderTest, TwoFramesLosslessPixelIdentical) {
    auto config = anim_config({frame(make_solid_rgba(128, 128, 200, 30, 30, 255)),
                               frame(make_solid_rgba(128, 128, 30, 30, 200, 255), 100, 1)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    // Container shape: ANIMATED flag, 2 frames, tile_offset 0 and 4
    // (128px canvas / 64px tiles = 2x2 tiles per full-canvas frame).
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_TRUE(parsed->header.is_animated());
    ASSERT_TRUE(parsed->animation.has_value());
    EXPECT_EQ(parsed->animation->frame_count, 2u);
    ASSERT_EQ(parsed->animation->frames.size(), 2u);
    EXPECT_EQ(parsed->animation->frames[0].tile_offset, 0u);
    EXPECT_EQ(parsed->animation->frames[1].tile_offset, 4u);
    EXPECT_EQ(parsed->tile_chunks.size(), 8u);

    auto frame0 = decode_frame(*encoded, 0);
    ASSERT_TRUE(frame0.has_value()) << frame0.error().message;
    expect_images_equal(*frame0, make_solid_rgba(128, 128, 200, 30, 30, 255));
    auto frame1 = decode_frame(*encoded, 1);
    ASSERT_TRUE(frame1.has_value()) << frame1.error().message;
    expect_images_equal(*frame1, make_solid_rgba(128, 128, 30, 30, 200, 255));
}

TEST(AnimationEncoderTest, LossyFramesMatchStillEncodes) {
    auto red = make_solid_rgb(128, 128, 200, 30, 30);
    auto blue = make_solid_rgb(128, 128, 30, 30, 200);
    auto config = anim_config({frame(red), frame(blue)});
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;

    // Each frame decodes exactly like the same frame encoded as a still
    // with the same config: the frame's tiles are the still's tiles.
    EncoderConfig still_config;
    still_config.tile_size_log2 = 6;
    auto still0 = encode(red, still_config);
    ASSERT_TRUE(still0.has_value());
    auto decoded0 = decode(*still0);
    ASSERT_TRUE(decoded0.has_value());
    auto frame0 = decode_frame(*encoded, 0);
    ASSERT_TRUE(frame0.has_value()) << frame0.error().message;
    expect_images_equal(*frame0, *decoded0);

    auto still1 = encode(blue, still_config);
    ASSERT_TRUE(still1.has_value());
    auto decoded1 = decode(*still1);
    ASSERT_TRUE(decoded1.has_value());
    auto frame1 = decode_frame(*encoded, 1);
    ASSERT_TRUE(frame1.has_value()) << frame1.error().message;
    expect_images_equal(*frame1, *decoded1);
}

TEST(AnimationEncoderTest, OverBlendsAlpha) {
    // Lossless, bit-exact: opaque red frame 0, semi-transparent blue
    // frame 1 OVER it. Hand-computed: (0*128 + 255*127 + 127)/255 = 127
    // for R, (255*128 + 127)/255 = 128 for B, A = 255.
    auto config = anim_config({frame(make_solid_rgba(128, 128, 255, 0, 0, 255)),
                               frame(make_solid_rgba(128, 128, 0, 0, 255, 128))},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto frame1 = decode_frame(*encoded, 1);
    ASSERT_TRUE(frame1.has_value()) << frame1.error().message;
    EXPECT_TRUE(frame1->has_alpha());
    expect_pixel(*frame1, 0, 0, 127, 0, 128, 255);
    expect_pixel(*frame1, 127, 127, 127, 0, 128, 255);
}

TEST(AnimationEncoderTest, SourceReplaces) {
    auto config = anim_config({frame(make_solid_rgba(128, 128, 255, 0, 0, 255)),
                               frame(make_solid_rgba(128, 128, 0, 0, 255, 128), 100, 1)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto frame1 = decode_frame(*encoded, 1);
    ASSERT_TRUE(frame1.has_value()) << frame1.error().message;
    expect_pixel(*frame1, 0, 0, 0, 0, 255, 128);
    expect_pixel(*frame1, 127, 127, 0, 0, 255, 128);
}

TEST(AnimationEncoderTest, SubRectOnlyChangesRect) {
    // Frame 1 is a 64x64 sub-rect at (64, 0): only that quadrant changes.
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(64, 64, 30, 30, 200), 100, 1, 0, 64, 0)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto frame1 = decode_frame(*encoded, 1);
    ASSERT_TRUE(frame1.has_value()) << frame1.error().message;
    expect_region(*frame1, 64, 0, 64, 64, 30, 30, 200);
    expect_region(*frame1, 0, 0, 64, 64, 200, 30, 30);
    expect_region(*frame1, 0, 64, 128, 64, 200, 30, 30);
}

TEST(AnimationEncoderTest, KeepLeavesPixels) {
    // Frame 1 paints the top strip blue (keep); frame 2 paints the
    // bottom-left quadrant green (keep). Frame 2 still shows both.
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(128, 64, 30, 30, 200), 100, 1, 0, 0, 0),
                               frame(make_solid_rgb(64, 64, 30, 200, 30), 100, 1, 0, 0, 64)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto frame2 = decode_frame(*encoded, 2);
    ASSERT_TRUE(frame2.has_value()) << frame2.error().message;
    expect_region(*frame2, 0, 0, 128, 64, 30, 30, 200);
    expect_region(*frame2, 0, 64, 64, 64, 30, 200, 30);
    expect_region(*frame2, 64, 64, 64, 64, 200, 30, 30);
}

TEST(AnimationEncoderTest, RestoreBgClearsRect) {
    // Frame 1 paints a quadrant blue then disposes to background
    // (transparent black 0x00000000): frame 2 shows background there.
    auto config = anim_config({frame(make_solid_rgba(128, 128, 200, 30, 30, 255)),
                               frame(make_solid_rgba(64, 64, 30, 30, 200, 255), 100, 1, 1, 64, 0),
                               frame(make_solid_rgba(64, 64, 30, 200, 30, 255), 100, 1, 0, 0, 64)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto frame1 = decode_frame(*encoded, 1);
    ASSERT_TRUE(frame1.has_value()) << frame1.error().message;
    expect_region(*frame1, 64, 0, 64, 64, 30, 30, 200, 255);
    auto frame2 = decode_frame(*encoded, 2);
    ASSERT_TRUE(frame2.has_value()) << frame2.error().message;
    expect_region(*frame2, 64, 0, 64, 64, 0, 0, 0, 0);
    expect_region(*frame2, 0, 64, 64, 64, 30, 200, 30, 255);
    expect_region(*frame2, 0, 0, 64, 64, 200, 30, 30, 255);
}

TEST(AnimationEncoderTest, DelayLoopBackgroundRoundTrip) {
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30), 100),
                               frame(make_solid_rgb(128, 128, 30, 30, 200), 250)},
                              3, 0x11223344u, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_TRUE(parsed->animation.has_value());
    EXPECT_EQ(parsed->animation->frame_count, 2u);
    EXPECT_EQ(parsed->animation->loop_count, 3u);
    EXPECT_EQ(parsed->animation->background_rgba, 0x11223344u);
    ASSERT_EQ(parsed->animation->frames.size(), 2u);
    EXPECT_EQ(parsed->animation->frames[0].delay_ms, 100u);
    EXPECT_EQ(parsed->animation->frames[1].delay_ms, 250u);
}

TEST(AnimationEncoderTest, RejectsEmptyFrames) {
    EncoderConfig config;
    config.tile_size_log2 = 6;
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsZeroSizeFrame) {
    auto config = anim_config({frame(Image(0, 128, BitDepth::Bits8, false))}, 0, 0, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsMismatchedBitDepth) {
    // A 10-bit frame under the default 8-bit config must fail, not
    // misread the pixels at the wrong stride.
    auto config = anim_config({frame(Image(128, 128, BitDepth::Bits10, false))}, 0, 0, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsRectOutsideCanvas) {
    // Frame 1 rect (64, 0, 128, 64) runs past the 128px canvas.
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(128, 64, 30, 30, 200), 100, 1, 0, 64, 0)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsRectNotTileAligned) {
    // Frame 1 rect_x = 32 is not a multiple of the 64px tile size.
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(64, 64, 30, 30, 200), 100, 1, 0, 32, 0)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsBadBlendMode) {
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(128, 128, 30, 30, 200), 100, 2)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsBadDisposal) {
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(128, 128, 30, 30, 200), 100, 0, 2)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, RejectsLosslessSingleTileSubRectAtOrigin) {
    // Lossless frame 1 is a single 64px tile at canvas (0, 0) with a
    // sub-rect: the decoder reads a single (0, 0) tile as the whole
    // frame (SPEC 4.4), so the encoder must refuse to write it.
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30)),
                               frame(make_solid_rgb(64, 64, 30, 30, 200), 100, 1, 0, 0, 0)},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}

TEST(AnimationEncoderTest, SingleFrameIsAnimated) {
    auto config = anim_config({frame(make_solid_rgb(128, 128, 200, 30, 30))}, 0, 0, true);
    auto encoded = encode_animation(config);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
    auto parsed = parse_container(*encoded);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    EXPECT_TRUE(parsed->header.is_animated());
    ASSERT_TRUE(parsed->animation.has_value());
    EXPECT_EQ(parsed->animation->frame_count, 1u);
    auto frame0 = decode_frame(*encoded, 0);
    ASSERT_TRUE(frame0.has_value()) << frame0.error().message;
    expect_images_equal(*frame0, make_solid_rgb(128, 128, 200, 30, 30));
}

TEST(AnimationEncoderTest, RejectsFrame0WithOffset) {
    // v1 rule: frame 0 defines the canvas and must sit at the origin.
    auto config = anim_config({frame(make_solid_rgb(64, 64, 200, 30, 30), 100, 0, 0, 32, 0),
                               frame(make_solid_rgb(64, 64, 30, 30, 200))},
                              0, 0x00000000u, true);
    auto encoded = encode_animation(config);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, ErrorCode::InvalidParameter);
}
