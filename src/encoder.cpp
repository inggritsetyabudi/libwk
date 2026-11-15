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
#include "adaptive_chroma.h"
#include "coeff_span_stream.h"
#include "container.h"
#include "cclm.h"
#include "control_field.h"
#include "superblock_quant.h"
#include "dct.h"
#include "lossy_coeff_stream.h"
#include "mode_span_rans.h"
#include "mode_stream.h"
#include "quantize.h"
#include "predict.h"
#include "colorspace.h"
#include "lossless.h"
#include "lossy_tile_encode.h"
#include "threading.h"
#include <wk/wk.hpp>
#include <wk/wk.h>
#include <wk/wkmeta.hpp>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <exception>
#include <new>
#include <optional>

namespace wk {


// Writes one V7 sparse quant-scale list: uint16 count, then
// (block_index uint16, scale_index uint8) per non-identity block, in
// raster order (strictly increasing by construction). A tile holds at
// most 1024x1024 pixels, so block indices always fit in uint16.

static Result<uint8_t> resolve_tile_size_log2(const EncoderConfig& config) {
    if (config.tile_size_log2 != 0) {
        if (config.tile_size_log2 < 6 || config.tile_size_log2 > 10) {
            return std::unexpected(Error{ErrorCode::InvalidParameter,
                "tile_size_log2 must be 0 or between 6 and 10"});
        }
        return config.tile_size_log2;
    }

    if (config.lossless) {
        return static_cast<uint8_t>(9);
    }

    return static_cast<uint8_t>(10);
}


// (clamp_quantized_to_stream_range lives in lossy_coeff_stream.h,
// next to coeff_symbol_offset, so the RDO can use it too.)

static std::vector<uint8_t> expand_rgb8_to_rgba8(const Image& image) {
    std::vector<uint8_t> expanded(static_cast<size_t>(image.width()) * image.height() * 4, 255);
    const auto pixels = image.pixels();
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    for (size_t i = 0; i < pixel_count; ++i) {
        expanded[i * 4 + 0] = pixels[i * 3 + 0];
        expanded[i * 4 + 1] = pixels[i * 3 + 1];
        expanded[i * 4 + 2] = pixels[i * 3 + 2];
    }
    return expanded;
}

// A tile is a smooth chroma ramp when its chroma spans a wide range
// (mean_deviation) yet changes only gradually (both gradients small) with
// no strong color edges. Only such tiles get the finer low-frequency
// chroma steps: on flat chroma the refinement is pure waste, and on
// textured content it costs bytes and can flip RDO modes for the worse.
// Thresholds measured against the 6 benchmark photos plus synthetic
// gradient/gray/solid tiles -- the ramp is the only tile that passes all
// five, with ~2-3x margin on each.


Result<std::vector<uint8_t>> encode_impl(const Image& image, const EncoderConfig& config,
                                         EncodeStats* stats = nullptr) {
    uint32_t width = image.width();
    uint32_t height = image.height();

    if (width == 0 || height == 0) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "zero dimensions"});
    }

    // The decoder rejects CICP values the converter does not implement
    // (see parse_fhdr), so the encoder must not produce them either.
    if (config.cicp.matrix != 1 && config.cicp.matrix != 5 && config.cicp.matrix != 6 && config.cicp.matrix != 9) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "unknown CICP matrix coefficient"});
    }
    if (config.cicp.transfer != 1 && config.cicp.transfer != 13 && config.cicp.transfer != 16 && config.cicp.transfer != 18) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "unknown CICP transfer characteristic"});
    }

    auto tile_size_log2 = resolve_tile_size_log2(config);
    if (!tile_size_log2) {
        return std::unexpected(tile_size_log2.error());
    }

    const bool source_has_alpha = image.has_alpha();

    FrameHeader fhdr;
    fhdr.width = width;
    fhdr.height = height;
    fhdr.bit_depth = static_cast<uint8_t>(config.bit_depth);
    fhdr.cicp_primaries = config.cicp.primaries;
    fhdr.cicp_transfer = config.cicp.transfer;
    fhdr.cicp_matrix = config.cicp.matrix;
    fhdr.tile_size_log2 = *tile_size_log2;

    fhdr.flags = 0;
    if (config.lossless) fhdr.flags |= wk::FHDR_FLAG_LOSSLESS;
    if (source_has_alpha) fhdr.flags |= wk::FHDR_FLAG_ALPHA;
    if (config.bit_depth != BitDepth::Bits8) fhdr.flags |= wk::FHDR_FLAG_HDR;
    if (config.cicp.full_range) fhdr.flags |= wk::FHDR_FLAG_FULL_RANGE;
    fhdr.flags |= FHDR_FLAG_TILED;

    WkFile file;
    file.header = fhdr;

    if (config.lossless) {
        std::vector<uint8_t> expanded_rgb;
        const uint8_t* lossless_pixels = image.pixels().data();
        if (!source_has_alpha && config.bit_depth == BitDepth::Bits8) {
            expanded_rgb = expand_rgb8_to_rgba8(image);
            lossless_pixels = expanded_rgb.data();
        }

        auto encoded = lossless_encode(lossless_pixels, width, height, fhdr.bit_depth);
        if (!encoded) return std::unexpected(encoded.error());

        Chunk tile_chunk;
        std::memcpy(tile_chunk.type, CHUNK_TILE, 4);

        TileHeader th;
        th.tile_x = 0;
        th.tile_y = 0;
        th.layer_flags = TILE_HAS_BASE;
        th.compressed_size = static_cast<uint32_t>(encoded->size());

        ByteWriter tw;
        auto th_data = serialize_tile_header(th);
        tw.write_bytes(th_data);
        tw.write_bytes(*encoded);
        tile_chunk.payload = tw.finish();

        file.tile_chunks.push_back(std::move(tile_chunk));
    } else {
        size_t pixel_count = static_cast<size_t>(width) * height;

        std::vector<int16_t> y_plane(pixel_count);
        std::vector<int16_t> cb_plane_full(pixel_count);
        std::vector<int16_t> cr_plane_full(pixel_count);

        rgb_to_ycbcr(y_plane.data(), cb_plane_full.data(), cr_plane_full.data(),
                      image.pixels().data(), width, height,
                      fhdr.cicp_matrix, fhdr.bit_depth, fhdr.full_range(),
                      source_has_alpha);

        std::vector<int16_t> cb_plane;
        std::vector<int16_t> cr_plane;
        std::vector<int16_t> alpha_plane;
        if (source_has_alpha) {
            alpha_plane.resize(pixel_count);
            if (fhdr.bit_depth == 8) {
                const auto pixels = image.pixels();
                for (size_t i = 0; i < pixel_count; ++i) {
                    alpha_plane[i] = pixels[i * 4 + 3];
                }
            } else {
                const uint16_t* pixels = reinterpret_cast<const uint16_t*>(image.pixels().data());
                for (size_t i = 0; i < pixel_count; ++i) {
                    alpha_plane[i] = static_cast<int16_t>(pixels[i * 4 + 3]);
                }
            }
        }

        // YUV420 is undecodable when the first tile is 8x8 or smaller:
        // the decoder infers subsampling from the first tile's chroma
        // block counts, and at <=8px the 4:4:4 and 4:2:0 counts coincide,
        // so it defaults to 4:4:4 and decodes the chroma wrong. FHDR
        // carries no subsampling field, so for tiny images fall back to
        // 4:4:4 instead of writing a file no decoder can read correctly.
        // Normal-size images are unaffected.
        const bool use_yuv420 =
            config.subsampling == Subsampling::YUV420 && (width > 8 || height > 8);

        // Adaptive subsampling applies only when the image is not
        // already image-wide 420 (then every tile is 420 by definition).
        const bool use_adaptive_chroma =
            config.adaptive_chroma_subsampling && config.subsampling == Subsampling::YUV444;

        // The 420 planes are built image-wide (identical to the
        // image-wide 420 path) so a 420 tile's chroma matches what
        // --yuv420 would have written for that region.
        std::vector<int16_t> cb_plane_420, cr_plane_420;
        if (use_yuv420 || use_adaptive_chroma) {
            const uint32_t chroma_w = (width + 1) / 2;
            const uint32_t chroma_h = (height + 1) / 2;
            cb_plane_420.resize(chroma_w * chroma_h);
            cr_plane_420.resize(chroma_w * chroma_h);
            subsample_420(cb_plane_full.data(), cb_plane_420.data(), width, height);
            subsample_420(cr_plane_full.data(), cr_plane_420.data(), width, height);
        }

        if (use_yuv420) {
            cb_plane = std::move(cb_plane_420);
            cr_plane = std::move(cr_plane_420);
        } else {
            cb_plane = std::move(cb_plane_full);
            cr_plane = std::move(cr_plane_full);
        }

        uint32_t tile_size = fhdr.tile_size();
        uint32_t num_tx = fhdr.tiles_x();
        uint32_t num_ty = fhdr.tiles_y();

        ThreadPool pool(config.threads);
        std::vector<std::future<Result<TileEncodeResult>>> futures;

        for (uint32_t ty = 0; ty < num_ty; ty++) {
            for (uint32_t tx = 0; tx < num_tx; tx++) {
                auto future = pool.submit([&, tx, ty]() {
                    // Stats are collected only when the caller asked for
                    // them; otherwise encode_lossy_tile skips the work.
                    TileStats tile_stats{};
                    TileStats* tile_stats_ptr = (stats != nullptr) ? &tile_stats : nullptr;
                    ChromaSubsampling tile_subsampling =
                        use_yuv420 ? ChromaSubsampling::YUV420 : ChromaSubsampling::YUV444;
                    const int16_t* tile_cb = cb_plane.data();
                    const int16_t* tile_cr = cr_plane.data();
                    if (use_adaptive_chroma) {
                        // Per-tile decision from the source chroma
                        // (cb_plane is full-resolution here, since the
                        // image is not image-wide 420).
                        const uint32_t x0 = tx * tile_size;
                        const uint32_t y0 = ty * tile_size;
                        const uint32_t tw = std::min(tile_size, width - x0);
                        const uint32_t th = std::min(tile_size, height - y0);
                        tile_subsampling = should_subsample_chroma_420(
                            cb_plane.data(), cr_plane.data(), width, height,
                            x0, y0, tw, th,
                            static_cast<double>(config.adaptive_chroma_laplacian_threshold))
                            ? ChromaSubsampling::YUV420 : ChromaSubsampling::YUV444;
                        if (tile_subsampling == ChromaSubsampling::YUV420) {
                            tile_cb = cb_plane_420.data();
                            tile_cr = cr_plane_420.data();
                        }
                    }
                    return encode_lossy_tile(
                        y_plane.data(), tile_cb, tile_cr,
                        alpha_plane.empty() ? nullptr : alpha_plane.data(),
                        width, height, tx, ty, tile_size,
                        config.quality, fhdr.bit_depth,
                        source_has_alpha,
                        tile_subsampling,
                        config.use_control_field,
                        config.use_cclm,
                        use_adaptive_chroma,
                        tile_stats_ptr);
                });
                futures.push_back(std::move(future));
            }
        }

        float total_score = 0;
        int tile_count = 0;

        for (auto& future : futures) {
            auto result = future.get();
            if (!result) return std::unexpected(result.error());

            if (stats != nullptr) {
                stats->tiles.push_back(result->stats);
            }

            Chunk tile_chunk;
            std::memcpy(tile_chunk.type, CHUNK_TILE, 4);

            TileHeader th;
            th.tile_x = result->tile_x;
            th.tile_y = result->tile_y;
            th.layer_flags = static_cast<uint8_t>(
                TILE_HAS_BASE | (source_has_alpha ? TILE_HAS_ALPHA : 0) |
                ((result->is_v11 && result->subsampling == ChromaSubsampling::YUV420)
                     ? TILE_CHROMA_420
                     : 0));
            th.compressed_size = static_cast<uint32_t>(result->compressed.size());

            ByteWriter tw;
            auto th_data = serialize_tile_header(th);
            tw.write_bytes(th_data);
            tw.write_bytes(result->compressed);
            tile_chunk.payload = tw.finish();

            file.tile_chunks.push_back(std::move(tile_chunk));
            total_score += result->quality_score;
            tile_count++;
        }

        if (tile_count > 0) {
            float avg_score = total_score / tile_count;
            meta::MetaBlock meta;
            meta.set(meta::Namespace::Rating, meta::rating::QUALITY_SCORE, avg_score);
            file.metadata = std::move(meta);
            file.header.flags |= wk::FHDR_FLAG_HAS_WKMETA;
        }
    }

    return write_container(file);
}

// Public API boundary: see the decode/get_info wrappers in decoder.cpp.
// Allocation failure must surface as a Result error, never escape.
Result<std::vector<uint8_t>> encode(const Image& image, const EncoderConfig& config) {
    try {
        return encode_impl(image, config);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while encoding"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, std::string("exception while encoding: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, "unknown exception while encoding"});
    }
}

Result<std::vector<uint8_t>> encode_with_stats(const Image& image, const EncoderConfig& config,
                                               EncodeStats& stats) {
    try {
        stats.tiles.clear();
        return encode_impl(image, config, &stats);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "out of memory while encoding"});
    } catch (const std::exception& e) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, std::string("exception while encoding: ") + e.what()});
    } catch (...) {
        return std::unexpected(Error{ErrorCode::EncodeFailed, "unknown exception while encoding"});
    }
}


Image::Image(uint32_t w, uint32_t h, BitDepth bd, bool alpha) {
    info_.width = w;
    info_.height = h;
    info_.bit_depth = bd;
    info_.has_alpha = alpha;

    size_t bpp = bytes_per_pixel();
    pixels_.resize(static_cast<size_t>(w) * h * bpp);
}

uint32_t Image::bytes_per_pixel() const {
    bool is_16 = (info_.bit_depth != BitDepth::Bits8);
    return (info_.has_alpha ? 4u : 3u) * (is_16 ? 2u : 1u);
}

size_t Image::stride() const {
    return static_cast<size_t>(info_.width) * bytes_per_pixel();
}

uint8_t* Image::row(uint32_t y) {
    return pixels_.data() + static_cast<size_t>(y) * stride();
}

const uint8_t* Image::row(uint32_t y) const {
    return pixels_.data() + static_cast<size_t>(y) * stride();
}

}


extern "C" {

void wk_encoder_config_init(wk_encoder_config_t* config) {
    config->quality = 75.0f;
    config->lossless = 0;
    config->bit_depth = 8;
    config->cicp_primaries = 1;
    config->cicp_transfer = 1;
    config->cicp_matrix = 1;
    config->tile_size_log2 = 0;
    config->threads = 0;
    config->target_ssimulacra2 = 0.0f;
    config->chroma_subsampling = 0;
    config->full_range = 1;
    config->adaptive_chroma_subsampling = 1;
    config->adaptive_chroma_laplacian_threshold = 2.1f;
}

wk_error_t wk_encode(
    const uint8_t* pixels, uint32_t width, uint32_t height,
    uint32_t pixel_stride, uint8_t bpp,
    const wk_encoder_config_t* config,
    uint8_t** out_data, size_t* out_size) {

    if (!pixels || !config || !out_data || !out_size) {
        return WK_ERROR_INVALID_PARAM;
    }
    if (width == 0 || height == 0) {
        return WK_ERROR_INVALID_PARAM;
    }

    // bit_depth is the caller's claim about the pixel data: anything other
    // than 8/10/12 used to be silently treated as 8-bit, encoding the
    // wrong data without telling the caller.
    if (config->bit_depth != 8 && config->bit_depth != 10 && config->bit_depth != 12) {
        return WK_ERROR_INVALID_PARAM;
    }

    // bpp must agree with bit depth: 3 (RGB) / 4 (RGBA) at 8-bit,
    // 6 / 8 at 10/12-bit. A mismatch used to make the row copies below
    // read/write past the Image rows -- e.g. bpp 6 at 8-bit copied twice
    // the row size (heap overflow).
    const bool is_16bit = config->bit_depth != 8;
    if (bpp != (is_16bit ? 6 : 3) && bpp != (is_16bit ? 8 : 4)) {
        return WK_ERROR_INVALID_PARAM;
    }

    bool has_alpha = (bpp == 4 || bpp == 8);
    const size_t row_bytes = static_cast<size_t>(width) * bpp;
    const size_t src_stride = pixel_stride > 0 ? pixel_stride : row_bytes;
    // An explicit stride smaller than the row would make the row copies
    // overlap. The API has no buffer-size parameter, so no bound on the
    // stride itself can be checked here -- a stride is just a row pitch.
    // The overflow guard below only matters on 32-bit targets: on
    // 64-bit, y < 2^32 and stride < 2^32, so (height-1)*stride can never
    // wrap size_t; on 32-bit it can, and this rejects it.
    if (pixel_stride > 0 && pixel_stride < row_bytes) {
        return WK_ERROR_INVALID_PARAM;
    }
    if (height > 1 && src_stride > (SIZE_MAX - row_bytes) / (height - 1)) {
        return WK_ERROR_INVALID_PARAM;
    }

    wk::BitDepth bd = wk::BitDepth::Bits8;
    if (config->bit_depth == 10) bd = wk::BitDepth::Bits10;
    else if (config->bit_depth == 12) bd = wk::BitDepth::Bits12;

    // The Image constructor allocates width*height*bpp bytes from
    // caller-supplied dimensions; guard it so a huge claim returns
    // WK_ERROR_OOM instead of letting bad_alloc escape extern "C"
    // (which would terminate the caller).
    std::optional<wk::Image> image;
    try {
        image.emplace(width, height, bd, has_alpha);
        for (uint32_t y = 0; y < height; y++) {
            std::memcpy(image->row(y), pixels + y * src_stride, row_bytes);
        }
    } catch (const std::bad_alloc&) {
        return WK_ERROR_OOM;
    } catch (...) {
        return WK_ERROR_ENCODE_FAIL;
    }

    wk::EncoderConfig enc_config;
    enc_config.quality = config->quality;
    enc_config.lossless = config->lossless != 0;
    enc_config.bit_depth = bd;
    enc_config.subsampling = config->chroma_subsampling ?
        wk::Subsampling::YUV420 : wk::Subsampling::YUV444;
    enc_config.cicp = {config->cicp_primaries, config->cicp_transfer,
                       config->cicp_matrix, config->full_range != 0};
    enc_config.tile_size_log2 = config->tile_size_log2;
    enc_config.threads = config->threads;
    enc_config.target_ssimulacra2 = config->target_ssimulacra2;
    enc_config.adaptive_chroma_subsampling = config->adaptive_chroma_subsampling != 0;
    enc_config.adaptive_chroma_laplacian_threshold = config->adaptive_chroma_laplacian_threshold;

    auto result = wk::encode(*image, enc_config);
    if (!result) {
        return WK_ERROR_ENCODE_FAIL;
    }

    *out_size = result->size();
    *out_data = static_cast<uint8_t*>(std::malloc(result->size()));
    if (!*out_data) return WK_ERROR_OOM;
    std::memcpy(*out_data, result->data(), result->size());

    return WK_OK;
}

wk_error_t wk_get_info(const uint8_t* data, size_t size, wk_image_info_t* info) {
    if (!data || !info) return WK_ERROR_INVALID_PARAM;

    auto result = wk::get_info({data, size});
    if (!result) return WK_ERROR_INVALID_DATA;

    info->width = result->width;
    info->height = result->height;
    info->bit_depth = static_cast<uint8_t>(result->bit_depth);
    info->cicp_primaries = result->cicp.primaries;
    info->cicp_transfer = result->cicp.transfer;
    info->cicp_matrix = result->cicp.matrix;
    info->flags = 0;
    if (result->is_lossless) info->flags |= wk::FHDR_FLAG_LOSSLESS;
    if (result->is_animated) info->flags |= wk::FHDR_FLAG_ANIMATED;
    if (result->has_alpha) info->flags |= wk::FHDR_FLAG_ALPHA;
    if (result->is_hdr) info->flags |= wk::FHDR_FLAG_HDR;
    if (result->has_wkmeta) info->flags |= wk::FHDR_FLAG_HAS_WKMETA;
    if (result->cicp.full_range) info->flags |= wk::FHDR_FLAG_FULL_RANGE;
    info->tile_size_log2 = static_cast<uint8_t>(std::bit_width(result->tile_size) - 1);
    info->max_cll = result->max_cll;
    info->max_fall = result->max_fall;
    info->has_alpha = result->has_alpha;
    info->is_lossless = result->is_lossless;
    info->is_animated = result->is_animated;
    info->is_hdr = result->is_hdr;
    info->frame_count = result->frame_count;

    return WK_OK;
}

void wk_free(void* ptr) {
    std::free(ptr);
}

const char* wk_version(void) {
    return "0.2.7";
}

}

