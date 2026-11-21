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

#include "avif_decode.h"
#include "avif/avif.h"
#include "common.h"
#include "dav1d/dav1d.h"
#include "dav1d/headers.h"
#include <cstring>
#include <optional>

namespace wk::avif {

bool has_avif_magic(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12) {
        return false;
    }
    return std::memcmp(bytes.data() + 4, "ftyp", 4) == 0 &&
           (std::memcmp(bytes.data() + 8, "avif", 4) == 0 ||
            std::memcmp(bytes.data() + 8, "avis", 4) == 0);
}

namespace {

uint32_t read_u32be(std::span<const uint8_t> bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

struct Box {
    size_t begin;  // first byte of the box (size field)
    size_t end;    // one past the last byte
};

// The box starting at offset within bytes[0, end), or nothing when
// the bytes end or the box is truncated/ill-formed. A size of 1 means
// a 64-bit largesize follows the type; a size of 0 means "to the end
// of the enclosing box".
std::optional<Box> next_box(std::span<const uint8_t> bytes, size_t offset, size_t end) {
    if (offset + 8 > end) {
        return std::nullopt;
    }
    uint64_t box_size = read_u32be(bytes, offset);
    size_t header = 8;
    if (box_size == 1) {
        if (offset + 16 > end) {
            return std::nullopt;
        }
        box_size = (static_cast<uint64_t>(read_u32be(bytes, offset + 8)) << 32) |
                   read_u32be(bytes, offset + 12);
        header = 16;
    } else if (box_size == 0) {
        box_size = end - offset;
    }
    if (box_size < header || offset + box_size > end) {
        return std::nullopt;
    }
    return Box{offset, offset + static_cast<size_t>(box_size)};
}

bool box_type_is(std::span<const uint8_t> bytes, const Box& box, const char* type) {
    return std::memcmp(bytes.data() + box.begin + 4, type, 4) == 0;
}

// An "infe" box (item information entry) with version 2 carries the
// item type at content + 8 (version/flags 4, item ID 2, protection 2).
bool infe_is_grid(std::span<const uint8_t> bytes, const Box& box) {
    const size_t content = box.begin + 8;
    if (content + 12 > box.end) {
        return false;
    }
    if (bytes[content] != 2) {
        return false;  // version 1 entries have no item type field
    }
    return std::memcmp(bytes.data() + content + 8, "grid", 4) == 0;
}

bool iinf_has_grid_item(std::span<const uint8_t> bytes, size_t begin, size_t end) {
    // iinf content: version/flags (4), then entry count (2 bytes for
    // version 0, 4 for version 1+), then "infe" boxes.
    if (begin + 4 > end) {
        return false;
    }
    const size_t offset = begin + 4 + (bytes[begin] == 0 ? 2 : 4);
    size_t cursor = offset;
    while (auto box = next_box(bytes, cursor, end)) {
        if (box_type_is(bytes, *box, "infe") && infe_is_grid(bytes, *box)) {
            return true;
        }
        cursor = box->end;
    }
    return false;
}

bool meta_has_grid_item(std::span<const uint8_t> bytes, size_t begin, size_t end) {
    // meta content starts with version/flags (4), then child boxes.
    if (begin + 4 > end) {
        return false;
    }
    size_t cursor = begin + 4;
    while (auto box = next_box(bytes, cursor, end)) {
        if (box_type_is(bytes, *box, "iinf")) {
            return iinf_has_grid_item(bytes, box->begin + 8, box->end);
        }
        cursor = box->end;
    }
    return false;
}

// A grid AVIF stores its cells as separate items behind an "infe"
// entry of type "grid". libavif would decode it merged into one
// image; this slice rejects grids explicitly instead, so walk the
// boxes (ftyp -> meta -> iinf -> infe v2) and look for that type.
// Any truncation or ill-formed box means "not a grid" — the decoder
// below reports the real error.
bool has_grid_item(std::span<const uint8_t> bytes) {
    size_t cursor = 0;
    while (auto box = next_box(bytes, cursor, bytes.size())) {
        if (box_type_is(bytes, *box, "meta")) {
            return meta_has_grid_item(bytes, box->begin + 8, box->end);
        }
        cursor = box->end;
    }
    return false;
}

// ---- ispe vs AV1 frame dims (#ispe) ----
// A lying ispe makes the decoder scale the AV1 frame up to the ispe
// size (fuzz finding 1: ispe 16384x16384 on a 16x16 frame cost 33 s /
// 4.6 GB). Reject before NextImage when the ispe dims disagree with
// the AV1 sequence-header dims from the primary item's data.

// Content offset of a box: 8 for a normal header, 16 for largesize.
size_t box_content_begin(std::span<const uint8_t> bytes, const Box& box) {
    return box.begin + (read_u32be(bytes, box.begin) == 1 ? 16 : 8);
}

std::optional<Box> find_top_box(std::span<const uint8_t> bytes, const char* type) {
    size_t cursor = 0;
    while (auto box = next_box(bytes, cursor, bytes.size())) {
        if (box_type_is(bytes, *box, type)) return *box;
        cursor = box->end;
    }
    return std::nullopt;
}

std::optional<Box> find_child_box(std::span<const uint8_t> bytes, size_t begin, size_t end,
                                  const char* type) {
    size_t cursor = begin;
    while (auto box = next_box(bytes, cursor, end)) {
        if (box_type_is(bytes, *box, type)) return *box;
        cursor = box->end;
    }
    return std::nullopt;
}

// Primary item ID from pitm (version 0: U16, version 1: U32).
std::optional<uint32_t> primary_item_id(std::span<const uint8_t> bytes) {
    auto meta = find_top_box(bytes, "meta");
    if (!meta) return std::nullopt;
    const size_t content = box_content_begin(bytes, *meta);
    if (content + 4 > meta->end) return std::nullopt;
    auto pitm = find_child_box(bytes, content + 4, meta->end, "pitm");
    if (!pitm) return std::nullopt;
    const size_t c = box_content_begin(bytes, *pitm);
    if (c + 4 > pitm->end) return std::nullopt;
    if (bytes[c] == 0) {
        if (c + 6 > pitm->end) return std::nullopt;
        return (static_cast<uint32_t>(bytes[c + 4]) << 8) | bytes[c + 5];
    }
    if (bytes[c] == 1) {
        if (c + 8 > pitm->end) return std::nullopt;
        return read_u32be(bytes, c + 4);
    }
    return std::nullopt;
}

// (offset, length) of the first non-empty extent of the given item in
// iloc, following libavif's reader (read.c): version 0/1/2, item ID
// U16 (v<2) / U32 (v2), data_reference_index U16, base_offset, then
// extents. Only construction method 0 (file offset) is supported;
// anything else cannot be located here and fails the check.
std::optional<std::pair<uint64_t, uint64_t>> primary_item_extent(std::span<const uint8_t> bytes,
                                                                  uint32_t item_id) {
    auto meta = find_top_box(bytes, "meta");
    if (!meta) return std::nullopt;
    const size_t content = box_content_begin(bytes, *meta);
    if (content + 4 > meta->end) return std::nullopt;
    auto iloc = find_child_box(bytes, content + 4, meta->end, "iloc");
    if (!iloc) return std::nullopt;
    const size_t c = box_content_begin(bytes, *iloc);
    const size_t end = iloc->end;
    // version/flags (4) + offset/length sizes (1) + base/index sizes (1).
    if (c + 6 > end) return std::nullopt;
    const uint8_t version = bytes[c];
    if (version > 2) return std::nullopt;
    const size_t offset_size = bytes[c + 4] >> 4;
    const size_t length_size = bytes[c + 4] & 0xF;
    const size_t base_offset_size = bytes[c + 5] >> 4;
    const size_t index_size = bytes[c + 5] & 0xF;
    const auto size_ok = [](size_t s) { return s == 0 || s == 4 || s == 8; };
    if (!size_ok(offset_size) || !size_ok(length_size) || !size_ok(base_offset_size) ||
        !size_ok(index_size)) {
        return std::nullopt;
    }
    size_t o = c + 6;
    const auto read_field = [&](size_t n, uint64_t* out) -> bool {
        if (o + n > end) return false;
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | bytes[o + i];
        o += n;
        *out = v;
        return true;
    };
    uint64_t count = 0;
    if (!read_field(version == 2 ? 4 : 2, &count)) return std::nullopt;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t id = 0;
        if (!read_field(version == 2 ? 4 : 2, &id)) return std::nullopt;
        if (version >= 1) {
            // reserved(12) + construction_method(4).
            uint64_t method_field = 0;
            if (!read_field(2, &method_field)) return std::nullopt;
            if (id == item_id && (method_field & 0xF) != 0) return std::nullopt;
        }
        uint64_t data_ref = 0;
        if (!read_field(2, &data_ref)) return std::nullopt;
        uint64_t base_offset = 0;
        if (!read_field(base_offset_size, &base_offset)) return std::nullopt;
        uint64_t extent_count = 0;
        if (!read_field(2, &extent_count)) return std::nullopt;
        for (uint64_t e = 0; e < extent_count; ++e) {
            if (version >= 1 && index_size > 0) {
                uint64_t item_ref = 0;
                if (!read_field(index_size, &item_ref)) return std::nullopt;
            }
            uint64_t extent_offset = 0, extent_length = 0;
            if (!read_field(offset_size, &extent_offset)) return std::nullopt;
            if (!read_field(length_size, &extent_length)) return std::nullopt;
            if (id == item_id && extent_length > 0) {
                return std::pair<uint64_t, uint64_t>{base_offset + extent_offset, extent_length};
            }
        }
    }
    return std::nullopt;
}

// (max_width, max_height) of a sequence header OBU in the given OBU
// bytes, via dav1d's own parser; nothing when no sequence header is
// present or it does not parse.
std::optional<std::pair<uint32_t, uint32_t>> seq_header_dims(std::span<const uint8_t> obu) {
    if (obu.empty()) return std::nullopt;
    Dav1dSequenceHeader seq{};
    if (dav1d_parse_sequence_header(&seq, obu.data(), obu.size()) != 0) return std::nullopt;
    if (seq.max_width <= 0 || seq.max_height <= 0) return std::nullopt;
    return std::pair<uint32_t, uint32_t>{static_cast<uint32_t>(seq.max_width),
                                         static_cast<uint32_t>(seq.max_height)};
}

std::string ispe_mismatch_message(uint32_t ispe_w, uint32_t ispe_h, uint32_t av1_w,
                                  uint32_t av1_h) {
    return "AVIF ispe dimensions (" + std::to_string(ispe_w) + "x" + std::to_string(ispe_h) +
           ") disagree with AV1 frame dimensions (" + std::to_string(av1_w) + "x" +
           std::to_string(av1_h) + ")";
}

// Every av1C box in ipco whose configOBUs carry a sequence header
// must agree with the image dims; boxes without one are skipped.
// This is a secondary check only — it can never override the
// primary-item check below.
std::optional<Error> check_av1c_seq_headers(std::span<const uint8_t> bytes, uint32_t width,
                                            uint32_t height) {
    auto meta = find_top_box(bytes, "meta");
    if (!meta) return std::nullopt;
    const size_t content = box_content_begin(bytes, *meta);
    if (content + 4 > meta->end) return std::nullopt;
    auto iprp = find_child_box(bytes, content + 4, meta->end, "iprp");
    if (!iprp) return std::nullopt;
    size_t cursor = box_content_begin(bytes, *iprp);
    while (auto box = next_box(bytes, cursor, iprp->end)) {
        if (box_type_is(bytes, *box, "ipco")) {
            size_t c2 = box_content_begin(bytes, *box);
            while (auto av1c = next_box(bytes, c2, box->end)) {
                if (box_type_is(bytes, *av1c, "av1C")) {
                    const size_t cfg = box_content_begin(bytes, *av1c);
                    // AV1CodecConfigurationRecord is 4 bytes; the
                    // configOBUs follow it.
                    if (cfg + 4 < av1c->end) {
                        auto dims =
                            seq_header_dims(bytes.subspan(cfg + 4, av1c->end - (cfg + 4)));
                        if (dims && *dims != std::pair<uint32_t, uint32_t>{width, height}) {
                            return Error{ErrorCode::InvalidHeader,
                                         ispe_mismatch_message(width, height, dims->first,
                                                               dims->second)};
                        }
                    }
                }
                c2 = av1c->end;
            }
        }
        cursor = box->end;
    }
    return std::nullopt;
}

// ispe dims must match the AV1 frame dims from the sequence header in
// the primary item's data; otherwise NextImage would scale the frame
// up to the ispe size. An error is also returned when the frame dims
// cannot be verified at all — fail closed rather than decode blind.
std::optional<Error> check_ispe_matches_av1_frame(std::span<const uint8_t> bytes, uint32_t width,
                                                  uint32_t height) {
    const auto item_id = primary_item_id(bytes);
    if (!item_id) {
        return Error{ErrorCode::InvalidHeader,
                     "cannot verify AVIF frame dimensions from AV1 sequence header"};
    }
    const auto extent = primary_item_extent(bytes, *item_id);
    if (!extent || extent->first > bytes.size() || extent->second > bytes.size() - extent->first) {
        return Error{ErrorCode::InvalidHeader,
                     "cannot verify AVIF frame dimensions from AV1 sequence header"};
    }
    const auto dims = seq_header_dims(
        bytes.subspan(static_cast<size_t>(extent->first), static_cast<size_t>(extent->second)));
    if (!dims) {
        return Error{ErrorCode::InvalidHeader,
                     "cannot verify AVIF frame dimensions from AV1 sequence header"};
    }
    if (*dims != std::pair<uint32_t, uint32_t>{width, height}) {
        return Error{ErrorCode::InvalidHeader,
                     ispe_mismatch_message(width, height, dims->first, dims->second)};
    }
    return check_av1c_seq_headers(bytes, width, height);
}

// The carried CICP must describe what the RGB pixels actually are:
// libavif converts YUV->RGB with the file's matrix, and our encoder
// converts RGB->YCbCr with the carried matrix, so the two must agree.
// Normalization (documented in the slice report):
// - primaries: H.273 Table 2 entries pass through (the encoder
//   validates them the same way); anything else -> 1 (BT.709).
// - transfer: 1/13/16/18 pass through; unspecified (2) is the MIAF
//   default sRGB (13); anything else is rejected — the transfer
//   drives display, silently relabelling it would show wrong.
// - matrix: 1/5/6/9 pass through; unspecified (2) is what libavif
//   effectively uses (BT.601, MIAF default, colr.c); anything else
//   is rejected — libavif converts with that matrix's own
//   coefficients (FCC 4, SMPTE240 7) or rejects it outright, and
//   our encoder cannot represent it.
Result<CICP> normalize_cicp(uint8_t primaries, uint8_t transfer, uint8_t matrix) {
    CICP out;
    out.primaries = cicp_primaries_is_defined(primaries) ? primaries : 1;
    if (transfer == 2) {
        out.transfer = 13;
    } else if (transfer == 1 || transfer == 13 || transfer == 16 || transfer == 18) {
        out.transfer = transfer;
    } else {
        return std::unexpected(
            Error{ErrorCode::InvalidCICP, "unsupported AVIF transfer characteristics"});
    }
    if (matrix == 2) {
        out.matrix = 6;
    } else if (matrix == 1 || matrix == 5 || matrix == 6 || matrix == 9) {
        out.matrix = matrix;
    } else {
        return std::unexpected(
            Error{ErrorCode::InvalidCICP, "unsupported AVIF matrix coefficients"});
    }
    out.full_range = true;
    return out;
}

}  // namespace

Result<Image> decode(std::span<const uint8_t> bytes) {
    if (!has_avif_magic(bytes)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "not an AVIF file"});
    }
    if (has_grid_item(bytes)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "AVIF grid images not supported"});
    }

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) {
        return std::unexpected(Error{ErrorCode::OutOfMemory, "failed to create AVIF decoder"});
    }
    // Parse first so dimensions are known before any raster is
    // allocated (same caps as the container and the stb path: 65535
    // per side, 2^28 pixels).
    avifResult result = avifDecoderSetIOMemory(decoder, bytes.data(), bytes.size());
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return std::unexpected(
            Error{ErrorCode::InvalidHeader, std::string("failed to set AVIF input: ") + avifResultToString(result)});
    }
    result = avifDecoderParse(decoder);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return std::unexpected(
            Error{ErrorCode::InvalidHeader, std::string("failed to parse AVIF: ") + avifResultToString(result)});
    }
    if (decoder->imageCount > 1) {
        avifDecoderDestroy(decoder);
        return std::unexpected(Error{ErrorCode::InvalidHeader, "animated AVIF not supported"});
    }
    if (!decoder->image || decoder->image->width == 0 || decoder->image->height == 0 ||
        decoder->image->width > 65535 || decoder->image->height > 65535 ||
        static_cast<uint64_t>(decoder->image->width) * decoder->image->height > (1u << 28)) {
        avifDecoderDestroy(decoder);
        return std::unexpected(Error{ErrorCode::InvalidHeader, "AVIF dimensions out of range"});
    }
    if (decoder->image->depth != 8 && decoder->image->depth != 10) {
        avifDecoderDestroy(decoder);
        return std::unexpected(Error{ErrorCode::InvalidHeader, "unsupported AVIF bit depth"});
    }
    if (decoder->image->yuvFormat != AVIF_PIXEL_FORMAT_YUV444 &&
        decoder->image->yuvFormat != AVIF_PIXEL_FORMAT_YUV420) {
        avifDecoderDestroy(decoder);
        return std::unexpected(Error{ErrorCode::InvalidHeader, "unsupported AVIF chroma format"});
    }

    // ispe dims must match the AV1 frame dims, checked before
    // NextImage so a lying ispe cannot trigger a huge scale-up.
    if (auto err = check_ispe_matches_av1_frame(bytes, decoder->image->width,
                                                 decoder->image->height)) {
        avifDecoderDestroy(decoder);
        return std::unexpected(*err);
    }

    // NextImage decodes into decoder->image (owned by the decoder);
    // no separate avifImage is created or destroyed here.
    result = avifDecoderNextImage(decoder);
    if (result != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return std::unexpected(
            Error{ErrorCode::InvalidHeader, std::string("failed to decode AVIF: ") + avifResultToString(result)});
    }
    avifImage* image = decoder->image;

    auto cicp = normalize_cicp(static_cast<uint8_t>(image->colorPrimaries),
                               static_cast<uint8_t>(image->transferCharacteristics),
                               static_cast<uint8_t>(image->matrixCoefficients));
    if (!cicp) {
        avifDecoderDestroy(decoder);
        return std::unexpected(cicp.error());
    }

    const bool has_alpha = image->alphaPlane != nullptr;
    const uint32_t width = image->width;
    const uint32_t height = image->height;
    const BitDepth bit_depth =
        image->depth == 10 ? BitDepth::Bits10 : BitDepth::Bits8;

    // YUV->RGB with libavif's own conversion (AVIF_LIBYUV is off):
    // it applies the file's matrix and range, and upsamples 420
    // chroma (AUTOMATIC). The RGB pixels are full range, native
    // endian — the same layout Image stores.
    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, image);
    rgb.depth = image->depth;
    rgb.format = has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
    rgb.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_AUTOMATIC;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
        avifDecoderDestroy(decoder);
        return std::unexpected(Error{ErrorCode::OutOfMemory, "failed to allocate AVIF RGB buffer"});
    }
    result = avifImageYUVToRGB(image, &rgb);
    if (result != AVIF_RESULT_OK) {
        avifRGBImageFreePixels(&rgb);
        avifDecoderDestroy(decoder);
        return std::unexpected(
            Error{ErrorCode::InvalidHeader, std::string("failed to convert AVIF to RGB: ") + avifResultToString(result)});
    }

    Image out(width, height, bit_depth, has_alpha);
    const size_t channels = has_alpha ? 4u : 3u;
    const size_t bytes_per_sample = bit_depth == BitDepth::Bits10 ? 2u : 1u;
    const size_t row_bytes = static_cast<size_t>(width) * channels * bytes_per_sample;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.row(y), rgb.pixels + static_cast<size_t>(y) * rgb.rowBytes, row_bytes);
    }
    out.set_cicp(*cicp);
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return out;
}

}  // namespace wk::avif
