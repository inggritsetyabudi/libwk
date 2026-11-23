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

#include "common.h"
#include "container.h"
#include "mode_stream.h"
#include "colorspace.h"
#include "lossless.h"
#include "tile_util.h"
#include "lossy_tile_decode.h"
#include <wk/wk.hpp>
#include <wk/wk.h>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <optional>

namespace wk {


Result<ImageInfo> get_info_impl(std::span<const uint8_t> data) {
    auto file = parse_container(data);
    if (!file) return std::unexpected(file.error());

    ImageInfo info;
    info.width = file->header.width;
    info.height = file->header.height;
    info.bit_depth = static_cast<BitDepth>(file->header.bit_depth);
    info.cicp = {file->header.cicp_primaries, file->header.cicp_transfer,
                 file->header.cicp_matrix, file->header.full_range()};
    info.has_alpha = file->header.has_alpha();
    info.is_lossless = file->header.is_lossless();
    info.is_animated = file->header.is_animated();
    info.is_hdr = file->header.is_hdr();
    info.has_wkmeta = file->header.has_wkmeta();
    info.tile_size = file->header.tile_size();
    info.max_cll = file->header.max_cll;
    info.max_fall = file->header.max_fall;
    info.frame_count = file->animation ? file->animation->frame_count : 1;
    return info;
}

// Peeks the lossy layout tag of a tile payload (the quant tables plus
// the block dimensions precede it). Returns nullopt when the payload
// is too short to hold a tag; the tile decode reports that truncation.
[[nodiscard]] std::optional<uint32_t> peek_lossy_layout_tag(std::span<const uint8_t> tile_data) {
    constexpr size_t kLayoutTagOffset = 64 * sizeof(uint16_t) * 2 + 4 * sizeof(uint16_t);
    if (tile_data.size() < kLayoutTagOffset + sizeof(uint32_t)) {
        return std::nullopt;
    }
    uint32_t tag = 0;
    std::memcpy(&tag, tile_data.data() + kLayoutTagOffset, sizeof(tag));
    return tag;
}

// A frame's TILE chunks must cover its grid exactly once. Cheap
// (tile headers + a tiles_x*tiles_y seen map, at most 64 KiB): it
// must run before any frame-sized allocation, so a crafted header
// cannot turn a few bytes into a multi-GB raster (#hole8).
[[nodiscard]] Result<void> validate_tile_grid_coverage(const WkFile& file, const FrameHeader& hdr) {
    const uint32_t tiles_x = hdr.tiles_x();
    const uint32_t tiles_y = hdr.tiles_y();
    if (file.tile_chunks.size() != hdr.tile_count()) {
        return std::unexpected(Error{ErrorCode::DecodeFailed,
                                     "tile chunk count does not match frame grid"});
    }
    std::vector<char> seen(static_cast<size_t>(tiles_x) * tiles_y, 0);
    for (const auto& tile_chunk : file.tile_chunks) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(tile_chunk, tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());
        if (tile_header.tile_x >= tiles_x || tile_header.tile_y >= tiles_y) {
            return std::unexpected(Error{ErrorCode::InvalidTileCoord,
                                         "tile coordinates out of range"});
        }
        const size_t index = static_cast<size_t>(tile_header.tile_y) * tiles_x +
                             tile_header.tile_x;
        if (seen[index]) {
            return std::unexpected(Error{ErrorCode::InvalidTileCoord,
                                         "duplicate tile chunk"});
        }
        seen[index] = 1;
    }
    // Chunk count equals the grid size and every coordinate is unique,
    // so the grid is fully covered by the pigeonhole principle.
    return {};
}

// --- Slice 2: per-frame decode (SPEC Section 4) --------------------------
// background_rgba is 0xRRGGBBAA: red in the most significant byte, alpha
// in the least. SPEC Section 4 names the field but does not pin the byte
// order; this is the definition the decoder implements (see the report).
// #anim #fase9
struct BgRgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

[[nodiscard]] BgRgba split_background_rgba(uint32_t bg) {
    return {static_cast<uint8_t>(bg >> 24), static_cast<uint8_t>(bg >> 16),
            static_cast<uint8_t>(bg >> 8), static_cast<uint8_t>(bg)};
}

// Scales an 8-bit background byte to the image's sample depth.
[[nodiscard]] uint32_t bg_sample(uint8_t v, uint8_t bit_depth) {
    if (bit_depth == 8) return v;
    const uint32_t maxv = (1u << bit_depth) - 1;
    return (static_cast<uint32_t>(v) * maxv + 127) / 255;
}

void fill_pixels(uint8_t* dst, size_t pixel_count, const Image& image, BgRgba bg) {
    const uint8_t bit_depth = static_cast<uint8_t>(image.bit_depth());
    const size_t bpp = image.bytes_per_pixel();
    const bool is16 = image.bit_depth() != BitDepth::Bits8;
    const uint32_t r = bg_sample(bg.r, bit_depth);
    const uint32_t g = bg_sample(bg.g, bit_depth);
    const uint32_t b = bg_sample(bg.b, bit_depth);
    const uint32_t a = bg_sample(bg.a, bit_depth);
    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t* p = dst + i * bpp;
        if (is16) {
            uint16_t* s = reinterpret_cast<uint16_t*>(p);
            s[0] = static_cast<uint16_t>(r);
            s[1] = static_cast<uint16_t>(g);
            s[2] = static_cast<uint16_t>(b);
            if (image.has_alpha()) s[3] = static_cast<uint16_t>(a);
        } else {
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
            if (image.has_alpha()) p[3] = static_cast<uint8_t>(a);
        }
    }
}

void fill_rect(Image& image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, BgRgba bg) {
    const size_t bpp = image.bytes_per_pixel();
    for (uint32_t row = 0; row < h; ++row) {
        uint8_t* dst = image.row(y + row) + static_cast<size_t>(x) * bpp;
        fill_pixels(dst, w, image, bg);
    }
}

void fill_image(Image& image, BgRgba bg) {
    fill_pixels(image.pixels().data(),
                static_cast<size_t>(image.width()) * image.height(), image, bg);
}

// Reads one channel sample (0..maxv) from a pixel; alpha defaults to
// opaque when the image has none.
[[nodiscard]] uint32_t read_sample(const uint8_t* p, const Image& image, size_t channel) {
    if (image.bit_depth() != BitDepth::Bits8) {
        return reinterpret_cast<const uint16_t*>(p)[channel];
    }
    return p[channel];
}

void write_sample(uint8_t* p, const Image& image, size_t channel, uint32_t value) {
    if (image.bit_depth() != BitDepth::Bits8) {
        reinterpret_cast<uint16_t*>(p)[channel] = static_cast<uint16_t>(value);
    } else {
        p[channel] = static_cast<uint8_t>(value);
    }
}

// Straight-alpha Porter-Duff "over" (SPEC Section 4.3): the frame's
// pixels over the canvas. out_a = sa + da*(1-sa);
// out_c = (src_c*sa + dst_c*da*(1-sa)) / out_a. Integer math with
// 64-bit intermediates; a fully transparent result is (0,0,0,0).
// #anim #fase9
void composite_over(Image& canvas, const Image& frame, uint32_t rect_x, uint32_t rect_y) {
    const uint32_t maxv = (1u << static_cast<uint8_t>(canvas.bit_depth())) - 1;
    const size_t bpp = canvas.bytes_per_pixel();
    for (uint32_t row = 0; row < frame.height(); ++row) {
        uint8_t* dst_row = canvas.row(rect_y + row) + static_cast<size_t>(rect_x) * bpp;
        const uint8_t* src_row = frame.row(row);
        for (uint32_t col = 0; col < frame.width(); ++col) {
            uint8_t* d = dst_row + static_cast<size_t>(col) * bpp;
            const uint8_t* s = src_row + static_cast<size_t>(col) * bpp;
            const uint32_t sa = canvas.has_alpha() ? read_sample(s, canvas, 3) : maxv;
            const uint32_t da = canvas.has_alpha() ? read_sample(d, canvas, 3) : maxv;
            const uint64_t out_a = static_cast<uint64_t>(sa) +
                                   (static_cast<uint64_t>(da) * (maxv - sa) + maxv / 2) / maxv;
            if (out_a == 0) {
                for (size_t c = 0; c < (canvas.has_alpha() ? 4u : 3u); ++c) {
                    write_sample(d, canvas, c, 0);
                }
                continue;
            }
            for (size_t c = 0; c < 3; ++c) {
                const uint64_t num = static_cast<uint64_t>(read_sample(s, canvas, c)) * sa * maxv +
                                     static_cast<uint64_t>(read_sample(d, canvas, c)) * da * (maxv - sa);
                const uint64_t den = static_cast<uint64_t>(maxv) * out_a;
                write_sample(d, canvas, c, static_cast<uint32_t>((num + den / 2) / den));
            }
            if (canvas.has_alpha()) {
                write_sample(d, canvas, 3, static_cast<uint32_t>(out_a));
            }
        }
    }
}

// Composites one decoded frame rectangle onto the canvas (SPEC Section 4.3).
void composite_rect(Image& canvas, const Image& frame, const AnimFrame& anim_frame) {
    const size_t bpp = canvas.bytes_per_pixel();
    if (anim_frame.blend_mode == 1) {
        // SOURCE: the frame's pixels replace the canvas pixels in the rect.
        for (uint32_t row = 0; row < frame.height(); ++row) {
            uint8_t* dst = canvas.row(anim_frame.rect_y + row) +
                           static_cast<size_t>(anim_frame.rect_x) * bpp;
            std::memcpy(dst, frame.row(row), static_cast<size_t>(frame.width()) * bpp);
        }
        return;
    }
    composite_over(canvas, frame, anim_frame.rect_x, anim_frame.rect_y);
}

// Decodes one lossless tile holding the whole frame: the pre-0.2.8
// single-tile form, and the SPEC Section 4.4 lossless exception for an
// animated frame with exactly one TILE at (0, 0). The tile's coordinates
// must be (0, 0): the lossy path rejects out-of-range coordinates, and
// the lossless path must not silently accept a tile that claims to sit
// elsewhere.
Result<Image> decode_lossless_single_tile(const WkFile& file, size_t tile_index) {
    const auto& hdr = file.header;
    const uint32_t width = hdr.width;
    const uint32_t height = hdr.height;

    TileHeader header;
    auto tile_data = tile_payload_view(file.tile_chunks[tile_index], header);
    if (!tile_data) return std::unexpected(tile_data.error());
    if (header.tile_x != 0 || header.tile_y != 0) {
        return std::unexpected(Error{ErrorCode::InvalidTileCoord,
                                     "lossless tile must be at (0, 0)"});
    }

    // Decode first, allocate the output only once the payload proves
    // decodable (#hole8): a truncated single tile must fail before any
    // frame-sized raster exists.
    auto decoded = lossless_decode(*tile_data, width, height, hdr.bit_depth);
    if (!decoded) return std::unexpected(decoded.error());

    Image output(width, height, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    if (hdr.has_alpha()) {
        std::memcpy(output.pixels().data(), decoded->data(),
                    std::min(decoded->size(), output.pixels().size()));
    } else {
        const size_t pixel_count = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < pixel_count; ++i) {
            output.pixels()[i * 3 + 0] = (*decoded)[i * 4 + 0];
            output.pixels()[i * 3 + 1] = (*decoded)[i * 4 + 1];
            output.pixels()[i * 3 + 2] = (*decoded)[i * 4 + 2];
        }
    }
    return output;
}

// Decodes a run of lossless TILE chunks (tile_begin..tile_end) into a
// rect-sized raster. Tile coordinates are canvas coordinates; the tiles
// are placed at (tile_x * tile_size - rect_x, tile_y * tile_size -
// rect_y). The caller guarantees the run covers the rect exactly once
// (parse_container's validate_animation for frames, the grid-coverage
// check for still images).
Result<Image> decode_lossless_region(const WkFile& file, size_t tile_begin, size_t tile_end,
                                     uint32_t rect_x, uint32_t rect_y,
                                     uint32_t rect_w, uint32_t rect_h) {
    const auto& hdr = file.header;
    Image output(rect_w, rect_h, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    const uint32_t tile_size = hdr.tile_size();
    for (size_t i = tile_begin; i < tile_end; ++i) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(file.tile_chunks[i], tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());
        const uint32_t tw = std::min(tile_size, hdr.width - tile_header.tile_x * tile_size);
        const uint32_t th = std::min(tile_size, hdr.height - tile_header.tile_y * tile_size);

        auto tile_decoded = lossless_decode(*tile_data, tw, th, hdr.bit_depth);
        if (!tile_decoded) return std::unexpected(tile_decoded.error());

        const uint32_t dst_x0 = tile_header.tile_x * tile_size - rect_x;
        const uint32_t dst_y0 = tile_header.tile_y * tile_size - rect_y;
        for (uint32_t y = 0; y < th; ++y) {
            uint8_t* dst_row = output.row(dst_y0 + y);
            for (uint32_t x = 0; x < tw; ++x) {
                const size_t src = (static_cast<size_t>(y) * tw + x) * 4;
                if (hdr.has_alpha()) {
                    uint8_t* dst = dst_row + (dst_x0 + x) * 4;
                    std::memcpy(dst, tile_decoded->data() + src, 4);
                } else {
                    uint8_t* dst = dst_row + (dst_x0 + x) * 3;
                    dst[0] = (*tile_decoded)[src + 0];
                    dst[1] = (*tile_decoded)[src + 1];
                    dst[2] = (*tile_decoded)[src + 2];
                }
            }
        }
    }
    return output;
}

// Decodes a run of lossy TILE chunks (tile_begin..tile_end) into a
// rect-sized raster. This is the still-image lossy path, scoped to a
// rectangle: the tile geometry stays canvas-based (tile_x * tile_size),
// the planes are rect-sized, and the 420 upsample runs on the rect's
// chroma (edge-clamped inside the rect — a frame is independent, SPEC
// Section 4.1, so its chroma never reads another frame's tiles). For
// the full canvas the rect is the canvas and the result is identical
// to the still-image decode.
Result<Image> decode_lossy_region(const WkFile& file, size_t tile_begin, size_t tile_end,
                                  uint32_t rect_x, uint32_t rect_y,
                                  uint32_t rect_w, uint32_t rect_h) {
    const auto& hdr = file.header;
    const uint32_t width = hdr.width;
    const uint32_t height = hdr.height;

    if (tile_begin >= tile_end) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "no tile chunks"});
    }

    // Every lossy tile starts with 256 bytes of quant tables + 8 bytes
    // of block dims + a 4-byte layout tag: anything shorter is provably
    // truncated, so reject it before the frame-wide planes are
    // allocated (#hole8).
    for (size_t i = tile_begin; i < tile_end; ++i) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(file.tile_chunks[i], tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());
        if (tile_data->size() < 268) {
            return std::unexpected(Error{ErrorCode::TruncatedInput,
                                         "lossy tile payload shorter than tile header"});
        }
    }

    const uint32_t tile_size = hdr.tile_size();
    TileHeader first_tile_header;
    auto first_tile_payload = tile_payload_view(file.tile_chunks[tile_begin], first_tile_header);
    if (!first_tile_payload) return std::unexpected(first_tile_payload.error());

    // Image-wide fallback subsampling, inferred from the first tile as
    // before. It is only the preferred value for pre-V11 tiles (whose
    // block counts are the sole signal); V11 tiles take their
    // subsampling from the tile header flag instead, so a mixed file
    // needs no single image-wide subsampling.
    auto subsampling = infer_subsampling_from_tile(*first_tile_payload, first_tile_header.tile_x,
                                                   first_tile_header.tile_y, tile_size,
                                                   width, height);
    if (!subsampling) return std::unexpected(subsampling.error());

    // A file is mixed (per-tile subsampling) when any tile uses the
    // V11 layout. Legacy files take the old path below, so they decode
    // bit-identically (one global upsample); mixed files composite
    // chroma at full resolution, upsampling each 420 tile to its own
    // region (SPEC v0.2.6).
    bool has_v11_tile = false;
    for (size_t i = tile_begin; i < tile_end; ++i) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(file.tile_chunks[i], tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());
        if (peek_lossy_layout_tag(*tile_data) == kLossyTileLayoutTagV11) {
            has_v11_tile = true;
            break;
        }
    }

    const int16_t max_val = static_cast<int16_t>((1 << hdr.bit_depth) - 1);
    const size_t pixel_count = static_cast<size_t>(rect_w) * rect_h;
    std::vector<int16_t> full_y(pixel_count, 0);

    const uint32_t chroma_w = has_v11_tile ? rect_w
        : (*subsampling == ChromaSubsampling::YUV420 ? (rect_w + 1) / 2 : rect_w);
    const uint32_t chroma_h = has_v11_tile ? rect_h
        : (*subsampling == ChromaSubsampling::YUV420 ? (rect_h + 1) / 2 : rect_h);
    std::vector<int16_t> full_cb(static_cast<size_t>(chroma_w) * chroma_h, max_val / 2);
    std::vector<int16_t> full_cr(static_cast<size_t>(chroma_w) * chroma_h, max_val / 2);
    std::vector<int16_t> full_a;
    if (hdr.has_alpha()) {
        full_a.assign(pixel_count, max_val);
    }

    for (size_t i = tile_begin; i < tile_end; ++i) {
        TileHeader tile_header;
        auto tile_data = tile_payload_view(file.tile_chunks[i], tile_header);
        if (!tile_data) return std::unexpected(tile_data.error());

        auto tile = decode_lossy_tile(*tile_data, tile_header.tile_x, tile_header.tile_y,
                                      tile_size, width, height, hdr.bit_depth, *subsampling,
                                      tile_header.layer_flags,
                                      hdr.has_alpha(), (tile_header.layer_flags & TILE_HAS_ALPHA) != 0);
        if (!tile) return std::unexpected(tile.error());

        const uint32_t dst_x0 = tile->geometry.x0 - rect_x;
        const uint32_t dst_y0 = tile->geometry.y0 - rect_y;
        for (uint32_t row = 0; row < tile->geometry.th; ++row) {
            for (uint32_t col = 0; col < tile->geometry.tw; ++col) {
                full_y[(dst_y0 + row) * rect_w + (dst_x0 + col)] =
                    tile->y_plane[row * tile->geometry.tw + col];
            }
        }

        if (hdr.has_alpha()) {
            for (uint32_t row = 0; row < tile->geometry.th; ++row) {
                for (uint32_t col = 0; col < tile->geometry.tw; ++col) {
                    full_a[(dst_y0 + row) * rect_w + (dst_x0 + col)] =
                        tile->alpha_plane[row * tile->geometry.tw + col];
                }
            }
        }

        if (has_v11_tile) {
            if (tile->geometry.subsampling == ChromaSubsampling::YUV420) {
                // Upsample this tile's chroma to its full-resolution
                // region (same bilinear filter as the image-wide
                // upsample, clamped to the tile's own chroma plane),
                // then copy it in.
                std::vector<int16_t> cb_up(static_cast<size_t>(tile->geometry.tw) * tile->geometry.th);
                std::vector<int16_t> cr_up(static_cast<size_t>(tile->geometry.tw) * tile->geometry.th);
                upsample_420(tile->cb_plane.data(), cb_up.data(), tile->geometry.tw, tile->geometry.th);
                upsample_420(tile->cr_plane.data(), cr_up.data(), tile->geometry.tw, tile->geometry.th);
                for (uint32_t row = 0; row < tile->geometry.th; ++row) {
                    for (uint32_t col = 0; col < tile->geometry.tw; ++col) {
                        full_cb[(dst_y0 + row) * rect_w + (dst_x0 + col)] =
                            cb_up[row * tile->geometry.tw + col];
                        full_cr[(dst_y0 + row) * rect_w + (dst_x0 + col)] =
                            cr_up[row * tile->geometry.tw + col];
                    }
                }
            } else {
                for (uint32_t row = 0; row < tile->geometry.chroma_th; ++row) {
                    for (uint32_t col = 0; col < tile->geometry.chroma_tw; ++col) {
                        full_cb[(dst_y0 + row) * rect_w + (dst_x0 + col)] =
                            tile->cb_plane[row * tile->geometry.chroma_tw + col];
                        full_cr[(dst_y0 + row) * rect_w + (dst_x0 + col)] =
                            tile->cr_plane[row * tile->geometry.chroma_tw + col];
                    }
                }
            }
        } else {
            const uint32_t chroma_x0 = *subsampling == ChromaSubsampling::YUV420 ? tile->geometry.x0 / 2 : tile->geometry.x0;
            const uint32_t chroma_y0 = *subsampling == ChromaSubsampling::YUV420 ? tile->geometry.y0 / 2 : tile->geometry.y0;
            const uint32_t rect_chroma_x0 = *subsampling == ChromaSubsampling::YUV420 ? rect_x / 2 : rect_x;
            const uint32_t rect_chroma_y0 = *subsampling == ChromaSubsampling::YUV420 ? rect_y / 2 : rect_y;
            for (uint32_t row = 0; row < tile->geometry.chroma_th; ++row) {
                for (uint32_t col = 0; col < tile->geometry.chroma_tw; ++col) {
                    full_cb[(chroma_y0 - rect_chroma_y0 + row) * chroma_w + (chroma_x0 - rect_chroma_x0 + col)] =
                        tile->cb_plane[row * tile->geometry.chroma_tw + col];
                    full_cr[(chroma_y0 - rect_chroma_y0 + row) * chroma_w + (chroma_x0 - rect_chroma_x0 + col)] =
                        tile->cr_plane[row * tile->geometry.chroma_tw + col];
                }
            }
        }
    }

    std::vector<int16_t> cb_full(pixel_count);
    std::vector<int16_t> cr_full(pixel_count);
    if (!has_v11_tile && *subsampling == ChromaSubsampling::YUV420) {
        upsample_420(full_cb.data(), cb_full.data(), rect_w, rect_h);
        upsample_420(full_cr.data(), cr_full.data(), rect_w, rect_h);
    } else {
        cb_full = std::move(full_cb);
        cr_full = std::move(full_cr);
    }

    // The output raster is allocated last: every tile has already
    // decoded into the planes above, so a truncated stream fails
    // before this allocation, never after it (#hole8).
    Image output(rect_w, rect_h, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    ycbcr_to_rgb(output.pixels().data(), full_y.data(), cb_full.data(), cr_full.data(),
                 rect_w, rect_h, hdr.cicp_matrix, hdr.bit_depth, hdr.full_range(),
                 hdr.has_alpha(), hdr.has_alpha() ? full_a.data() : nullptr);
    return output;
}

// Decodes one animation frame's own pixels (its rectangle, no canvas,
// no compositing): the frame's TILE chunks only, per SPEC Section 4.1.
Result<Image> decode_frame_pixels(const WkFile& file, uint32_t frame_index) {
    const auto& hdr = file.header;
    const AnimHeader& anim = *file.animation;
    const AnimFrame& frame = anim.frames[frame_index];
    const size_t begin = frame.tile_offset;
    const size_t end = (frame_index + 1 < anim.frames.size())
                           ? anim.frames[frame_index + 1].tile_offset
                           : file.tile_chunks.size();

    if (hdr.is_lossless()) {
        if (end - begin == 1) {
            TileHeader header;
            auto tile_data = tile_payload_view(file.tile_chunks[begin], header);
            if (!tile_data) return std::unexpected(tile_data.error());
            if (header.tile_x == 0 && header.tile_y == 0) {
                // SPEC Section 4.4 lossless exception: one TILE at
                // (0, 0) holding the whole frame; the rect is the full
                // canvas (validate_animation enforces it).
                return decode_lossless_single_tile(file, begin);
            }
        }
        return decode_lossless_region(file, begin, end, frame.rect_x, frame.rect_y,
                                      frame.rect_w, frame.rect_h);
    }
    return decode_lossy_region(file, begin, end, frame.rect_x, frame.rect_y,
                               frame.rect_w, frame.rect_h);
}

// The still-image decode: the pre-Slice-2 decode_impl body, unchanged
// in behaviour, now factored so decode_frame can reuse it for frame 0
// of a still image.
Result<Image> decode_still_image(const WkFile& file, const DecoderConfig& config) {
    const auto& hdr = file.header;
    const uint32_t width = hdr.width;
    const uint32_t height = hdr.height;

    if (config.info_only) {
        return Image(width, height, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    }

    if (hdr.is_lossless()) {
        // A lossless frame is either the pre-0.2.8 format (one tile at
        // (0,0) holding the whole frame) or the per-tile format (v0.2.8:
        // tile_count() tiles covering the grid exactly once). A single
        // tile at (0,0) is always read as the whole frame, so old files
        // decode unchanged; a file with any other tile count must cover
        // the grid exactly once, like the lossy path.
        if (file.tile_chunks.size() == 1) {
            return decode_lossless_single_tile(file, 0);
        }

        // Per-tile format: the TILE chunks must cover the frame grid
        // exactly once. A truncated file (fewer chunks than the grid)
        // fails here; a single tile payload decoded as a whole frame
        // above fails in the rANS/LZ stream instead -- either way it
        // fails loudly, never as wrong pixels. Validate before
        // allocating the output raster (#hole8).
        auto coverage = validate_tile_grid_coverage(file, hdr);
        if (!coverage) return std::unexpected(coverage.error());

        return decode_lossless_region(file, 0, file.tile_chunks.size(), 0, 0, width, height);
    }

    if (file.tile_chunks.empty()) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "no tile chunks"});
    }

    // A lossy frame must be covered by its TILE chunks exactly once:
    // a missing tile used to decode as a blank quadrant and a duplicated
    // tile silently overwrote the first copy. Check before decoding
    // anything, and before any frame-sized allocation, so a malformed
    // file fails fast instead of after gigabytes of work (#hole8).
    auto coverage = validate_tile_grid_coverage(file, hdr);
    if (!coverage) return std::unexpected(coverage.error());

    return decode_lossy_region(file, 0, file.tile_chunks.size(), 0, 0, width, height);
}

// Composited frame n (SPEC Section 4.3): the canvas after frames 0..n
// are composited in order, with the disposals of frames 0..n-1 applied
// between frames. A frame's own disposal never affects its own display:
// it applies after the frame's delay, i.e. before the next frame.
Result<Image> decode_frame_impl(const WkFile& file, uint32_t frame_index, const DecoderConfig& config) {
    const auto& hdr = file.header;
    const uint32_t width = hdr.width;
    const uint32_t height = hdr.height;

    if (!hdr.is_animated()) {
        if (frame_index != 0) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                                         "frame index out of range: still image has 1 frame"});
        }
        return decode_still_image(file, config);
    }

    const AnimHeader& anim = *file.animation;
    if (frame_index >= anim.frame_count) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "frame index out of range"});
    }

    if (config.info_only) {
        return Image(width, height, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    }

    const BgRgba bg = split_background_rgba(anim.background_rgba);
    Image canvas(width, height, static_cast<BitDepth>(hdr.bit_depth), hdr.has_alpha());
    fill_image(canvas, bg);
    for (uint32_t k = 0; k <= frame_index; ++k) {
        auto pixels = decode_frame_pixels(file, k);
        if (!pixels) return std::unexpected(pixels.error());
        const AnimFrame& frame = anim.frames[k];
        composite_rect(canvas, *pixels, frame);
        if (k < frame_index && frame.disposal == 1) {
            fill_rect(canvas, frame.rect_x, frame.rect_y, frame.rect_w, frame.rect_h, bg);
        }
    }
    return canvas;
}

Result<Image> decode_impl(std::span<const uint8_t> data, const DecoderConfig& config) {
    auto file = parse_container(data);
    if (!file) return std::unexpected(file.error());

    // An animated file decodes as its first composited frame: the old
    // behaviour (all TILEs stacked onto one raster, last tile per
    // coordinate winning) was the unfinished-animation placeholder, not
    // a defined semantic, and is replaced by frame 0.
    if (file->header.is_animated()) {
        return decode_frame_impl(*file, 0, config);
    }
    return decode_still_image(*file, config);
}

// Public API boundary: decode works on untrusted files, and allocation
// failure (std::bad_alloc / std::length_error) must surface as a Result
// error, never escape as an exception -- callers in C (wk_decode) and
// tools have no handler, so an escaping exception would terminate the
// process (SPEC section 10: the decoder must never crash on malformed
// input). The checks above already bound the known huge allocations;
// this is the last line of defense for any allocation left.
Result<ImageInfo> get_info(std::span<const uint8_t> data) {
    try {
        return get_info_impl(data);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while reading info"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, std::string("exception while reading info: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unknown exception while reading info"});
    }
}

Result<Image> decode(std::span<const uint8_t> data, const DecoderConfig& config) {
    try {
        return decode_impl(data, config);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while decoding"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, std::string("exception while decoding: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unknown exception while decoding"});
    }
}

// Public decode_frame: parses, then composites frame frame_index.
// #anim #fase9
Result<Image> decode_frame(std::span<const uint8_t> data, uint32_t frame_index,
                           const DecoderConfig& config) {
    try {
        auto file = parse_container(data);
        if (!file) return std::unexpected(file.error());
        return decode_frame_impl(*file, frame_index, config);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while decoding frame"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, std::string("exception while decoding frame: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::DecodeFailed, "unknown exception while decoding frame"});
    }
}

}

extern "C" {

wk_error_t wk_decode(
    const uint8_t* data, size_t size,
    uint8_t** out_pixels, uint32_t* out_width, uint32_t* out_height,
    uint8_t* out_bpp) {

    if (!data || !out_pixels || !out_width || !out_height || !out_bpp) {
        return WK_ERROR_INVALID_PARAM;
    }

    auto result = wk::decode({data, size});
    if (!result) return WK_ERROR_DECODE_FAIL;

    *out_width = result->width();
    *out_height = result->height();
    *out_bpp = static_cast<uint8_t>(result->bytes_per_pixel());

    size_t total = result->pixels().size();
    *out_pixels = static_cast<uint8_t*>(std::malloc(total));
    if (!*out_pixels) return WK_ERROR_OOM;
    std::memcpy(*out_pixels, result->pixels().data(), total);

    return WK_OK;
}

}

