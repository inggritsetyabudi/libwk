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
// distributed under the Apache License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jxl_decode.h"
#include "common.h"
#include <jxl/cms.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/color_encoding_internal.h>
#include <memory>
#include <vector>

namespace wk::jxl {

bool has_jxl_magic(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return false;
    }
    const JxlSignature sig = JxlSignatureCheck(bytes.data(), bytes.size());
    return sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER;
}

namespace {

// The carried CICP must describe what the RGB pixels actually are:
// libjxl decodes straight to RGB, and our encoder converts RGB to
// YCbCr with the carried matrix, so the two must agree.
// Normalization (mirrors the AVIF decoder, adapted for JXL — JXL
// carries no matrix, so it is derived from the primaries):
// - color space: RGB or gray only. XYB/unknown would mean the
//   output is not display-ready RGB; reject rather than mislabel.
// - primaries: H.273 Table 2 entries pass through (the encoder
//   validates them the same way); anything else -> 1 (BT.709).
//   CUSTOM (2) is a defined Table 2 value ("unspecified"), so it
//   passes as 2 — the explicit xy are lost, but that is the same
//   class of approximation as the AVIF decoder's mapping.
// - transfer: 1/13/16/18 pass through; unspecified (2) is the
//   sRGB default (13); anything else is rejected — the transfer
//   drives display, silently relabelling it would show wrong.
//   In particular LINEAR (8), DCI (17) and GAMMA (65535) are
//   rejected: there is no H.273 code for linear, and a gamma ICC
//   that libjxl reports as linear has already lost its gamma at
//   encode time (see tools/jxl_slice_2026-11-20.md).
// - matrix: JXL carries none, so it is derived from the primaries:
//   BT.2020 primaries -> 9, everything else -> 1 (the codec
//   default). The encoder uses this matrix for RGB->YCbCr, so
//   deriving it here keeps encode and decode consistent.
// - white point: not carried in CICP; any value is accepted and
//   not recorded. full_range is always true (JXL RGB is full
//   range).
Result<CICP> normalize_cicp(const JxlColorEncoding& enc) {
    if (enc.color_space != JXL_COLOR_SPACE_RGB &&
        enc.color_space != JXL_COLOR_SPACE_GRAY) {
        return std::unexpected(
            Error{ErrorCode::InvalidCICP, "unsupported JXL color space"});
    }
    CICP out;
    const uint32_t primaries = enc.primaries;
    out.primaries = (primaries <= 255 &&
                     cicp_primaries_is_defined(static_cast<uint8_t>(primaries)))
                        ? static_cast<uint8_t>(primaries)
                        : 1;
    const uint32_t transfer = enc.transfer_function;
    if (transfer == JXL_TRANSFER_FUNCTION_UNKNOWN) {
        out.transfer = 13;
    } else if (transfer == JXL_TRANSFER_FUNCTION_709 ||
               transfer == JXL_TRANSFER_FUNCTION_SRGB ||
               transfer == JXL_TRANSFER_FUNCTION_PQ ||
               transfer == JXL_TRANSFER_FUNCTION_HLG) {
        out.transfer = static_cast<uint8_t>(transfer);
    } else {
        return std::unexpected(Error{ErrorCode::InvalidCICP,
                                     "unsupported JXL transfer characteristics"});
    }
    out.matrix = (out.primaries == 9) ? 9 : 1;
    out.full_range = true;
    return out;
}

// Read the file's CICP. The enum path is primary: it succeeded for
// every file tested (including ffmpeg-encoded ones — the research
// report's claim that ffmpeg files need the ICC path came from a
// harness that called it at the wrong time, before the
// JXL_DEC_COLOR_ENCODING event). The ICC path below is a defensive
// fallback for profiles libjxl cannot express as fields: re-parse
// the raw profile with libjxl's own ICC->fields conversion.
Result<CICP> read_cicp(JxlDecoder* decoder) {
    JxlColorEncoding enc{};
    if (JxlDecoderGetColorAsEncodedProfile(decoder, JXL_COLOR_PROFILE_TARGET_DATA,
                                           &enc) == JXL_DEC_SUCCESS) {
        return normalize_cicp(enc);
    }
    size_t icc_size = 0;
    if (JxlDecoderGetICCProfileSize(decoder, JXL_COLOR_PROFILE_TARGET_DATA,
                                    &icc_size) != JXL_DEC_SUCCESS) {
        return std::unexpected(
            Error{ErrorCode::InvalidCICP, "JXL file has no usable color profile"});
    }
    std::vector<uint8_t> icc(icc_size);
    if (JxlDecoderGetColorAsICCProfile(decoder, JXL_COLOR_PROFILE_TARGET_DATA,
                                       icc.data(), icc.size()) != JXL_DEC_SUCCESS) {
        return std::unexpected(
            Error{ErrorCode::InvalidCICP, "failed to read JXL ICC profile"});
    }
    ::jxl::ColorEncoding color;
    if (!color.SetICC(::jxl::IccBytes(icc.begin(), icc.end()), JxlGetDefaultCms())) {
        return std::unexpected(Error{
            ErrorCode::InvalidCICP, "JXL ICC profile not representable as color fields"});
    }
    return normalize_cicp(color.ToExternal());
}

// libjxl scales integer output to the full range of the requested
// type: a 10-bit sample v becomes round(v * 65535 / 1023)
// (dec_external_image.cc: float in [0, 1] times 65535, nearest
// integer). The inverse below recovers every 10-bit value exactly:
// the forward rounding error is at most 1 LSB in 16-bit units,
// i.e. 0.016 in 10-bit units, far inside the inverse rounding
// window of +-0.5. Proven on the lossless fixtures (exact vs the
// source PPM) and on the lossless research files (byte-identical
// .wk through the encoder).
uint16_t unscale_sample(uint16_t v, uint32_t bits) {
    const uint32_t max_value = (1u << bits) - 1u;
    return static_cast<uint16_t>((static_cast<uint32_t>(v) * max_value + 32767) / 65535);
}

// Undo libjxl's 16-bit scaling in place. Color channels unscale by
// the image bit depth; the alpha channel unscales by its own
// precision first and is then rescaled to the image bit depth, so
// the Image stays internally consistent when the two differ (in
// practice they are equal and both steps are the identity).
void unscale_10bit(Image& image, uint32_t alpha_bits) {
    const uint32_t bits = 10;
    const size_t channels = image.has_alpha() ? 4u : 3u;
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    uint16_t* samples = reinterpret_cast<uint16_t*>(image.pixels().data());
    for (size_t i = 0; i < pixel_count; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            samples[i * channels + c] = unscale_sample(samples[i * channels + c], bits);
        }
        if (image.has_alpha()) {
            const uint16_t native =
                unscale_sample(samples[i * channels + 3], alpha_bits);
            const uint32_t alpha_max = (1u << alpha_bits) - 1u;
            const uint32_t image_max = (1u << bits) - 1u;
            samples[i * channels + 3] = static_cast<uint16_t>(
                (static_cast<uint32_t>(native) * image_max + alpha_max / 2) / alpha_max);
        }
    }
}

}  // namespace

Result<Image> decode(std::span<const uint8_t> bytes) {
    if (!has_jxl_magic(bytes)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "not a JXL file"});
    }
    JxlDecoder* raw = JxlDecoderCreate(nullptr);
    if (!raw) {
        return std::unexpected(
            Error{ErrorCode::OutOfMemory, "failed to create JXL decoder"});
    }
    const std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder(
        raw, &JxlDecoderDestroy);
    JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                                 JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(decoder.get(), bytes.data(), bytes.size());
    JxlDecoderCloseInput(decoder.get());

    JxlBasicInfo info{};
    bool have_info = false;
    bool have_cicp = false;
    CICP cicp{};
    uint32_t bits = 0;
    Image image;
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS) {
                return std::unexpected(
                    Error{ErrorCode::InvalidHeader, "failed to read JXL basic info"});
            }
            // Parse first so dimensions are known before any raster
            // is allocated (same caps as the container and the stb
            // path: 65535 per side, 2^28 pixels).
            if (info.xsize == 0 || info.ysize == 0 || info.xsize > 65535 ||
                info.ysize > 65535 ||
                static_cast<uint64_t>(info.xsize) * info.ysize > (1u << 28)) {
                return std::unexpected(
                    Error{ErrorCode::InvalidHeader, "JXL dimensions out of range"});
            }
            if (info.have_animation) {
                return std::unexpected(
                    Error{ErrorCode::InvalidHeader, "animated JXL not supported"});
            }
            if (info.bits_per_sample != 8 && info.bits_per_sample != 10) {
                return std::unexpected(
                    Error{ErrorCode::InvalidBitDepth, "unsupported JXL bit depth"});
            }
            if (info.num_color_channels != 1 && info.num_color_channels != 3) {
                return std::unexpected(
                    Error{ErrorCode::InvalidHeader, "unsupported JXL channel count"});
            }
            const uint32_t extra_non_alpha =
                info.num_extra_channels - (info.alpha_bits > 0 ? 1u : 0u);
            if (extra_non_alpha > 0) {
                return std::unexpected(
                    Error{ErrorCode::InvalidHeader, "unsupported JXL extra channel"});
            }
            // The alpha unscale below shifts by alpha_bits; cap it
            // at 16 (above anything the codec can represent).
            if (info.alpha_bits > 16) {
                return std::unexpected(Error{ErrorCode::InvalidBitDepth,
                                             "unsupported JXL alpha bit depth"});
            }
            have_info = true;
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            auto normalized = read_cicp(decoder.get());
            if (!normalized) {
                return std::unexpected(normalized.error());
            }
            cicp = *normalized;
            have_cicp = true;
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (!have_info || !have_cicp) {
                return std::unexpected(Error{
                    ErrorCode::InvalidHeader, "JXL output buffer needed before info"});
            }
            const bool has_alpha = info.alpha_bits > 0;
            const uint32_t channels = has_alpha ? 4u : 3u;
            bits = info.bits_per_sample;
            image = Image(info.xsize, info.ysize,
                          bits == 10 ? BitDepth::Bits10 : BitDepth::Bits8, has_alpha);
            // Gray is requested as 3 channels: libjxl replicates the
            // single channel to RGB itself (verified on the gray
            // fixture). 10-bit output is JXL_TYPE_UINT16, native
            // endian — decoded straight into the Image storage (same
            // layout, same size) and unscaled in place below.
            const JxlPixelFormat format{channels,
                                        bits == 10 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                                        JXL_NATIVE_ENDIAN, 0};
            if (JxlDecoderSetImageOutBuffer(decoder.get(), &format,
                                            image.pixels().data(),
                                            image.pixels().size()) != JXL_DEC_SUCCESS) {
                return std::unexpected(Error{ErrorCode::InvalidHeader,
                                             "failed to set JXL output buffer"});
            }
        } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
            if (image.pixels().empty()) {
                return std::unexpected(
                    Error{ErrorCode::InvalidHeader, "JXL decode produced no image"});
            }
            if (bits == 10) {
                unscale_10bit(image, info.alpha_bits);
            }
            image.set_cicp(cicp);
            return image;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            // Input was closed, so more input means truncation.
            return std::unexpected(
                Error{ErrorCode::InvalidHeader, "truncated JXL input"});
        } else if (status == JXL_DEC_ERROR) {
            return std::unexpected(
                Error{ErrorCode::InvalidHeader, "failed to decode JXL"});
        } else {
            return std::unexpected(
                Error{ErrorCode::InvalidHeader, "unexpected JXL decoder status"});
        }
    }
}

}  // namespace wk::jxl
