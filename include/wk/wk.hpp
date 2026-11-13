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
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace wk
{

    // ErrorCode and Error are defined here, in the public header, so that
    // <wk/wk.hpp> is self-contained: a translation unit that includes only
    // this header must be able to name the error type of the Result it
    // gets back from encode()/decode()/get_info(). They used to be only
    // forward-declared here and fully defined in the internal
    // src/common.h, which made any such use fail to compile with
    // "invalid use of incomplete type 'wk::Error'".
    enum class ErrorCode : uint32_t
    {
        Ok = 0,
        InvalidMagic,
        InvalidVersion,
        InvalidChunkType,
        InvalidChunkSize,
        TruncatedInput,
        InvalidHeader,
        InvalidTileCoord,
        InvalidBitDepth,
        InvalidCICP,
        UnsupportedFeature,
        EncodeFailed,
        DecodeFailed,
        RansError,
        DctError,
        PredictionError,
        QuantizeError,
        MetaParseError,
        MetaSerializeError,
        MetaTagNotFound,
        MetaTypeMismatch,
        InvalidParameter,
        IoError,
        OutOfMemory,
        ThreadingError,
        AnimationError,
        ColorSpaceError,
        LosslessError,
        ChecksumMismatch,
    };

    struct Error
    {
        ErrorCode code;
        std::string message;

        Error() = default;
        Error(ErrorCode c, std::string msg = {})
            : code(c), message(std::move(msg)) {}
        Error(const Error&) = default;
        Error(Error&&) = default;
        Error& operator=(const Error&) = default;
        Error& operator=(Error&&) = default;
        bool operator==(const Error& other) const {
            return code == other.code && message == other.message;
        }
    };

    template <typename T>
    using Result = std::expected<T, Error>;

    enum class BitDepth : uint8_t
    {
        Bits8 = 8,
        Bits10 = 10,
        Bits12 = 12
    };

    enum class Subsampling : uint8_t
    {
        YUV444 = 0,
        YUV420 = 1
    };

    struct CICP
    {
        uint8_t primaries = 1;
        uint8_t transfer = 1;
        uint8_t matrix = 1;
        bool full_range = true;
    };

    struct ImageInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        BitDepth bit_depth = BitDepth::Bits8;
        CICP cicp = {};
        bool has_alpha = false;
        bool is_lossless = false;
        bool is_animated = false;
        bool is_hdr = false;
        bool has_wkmeta = false;
        uint32_t tile_size = 512;
        uint16_t max_cll = 0;
        uint16_t max_fall = 0;
        uint32_t frame_count = 1;
    };

    class Image
    {
    public:
        Image() = default;
        Image(uint32_t w, uint32_t h, BitDepth bd = BitDepth::Bits8, bool alpha = true);

        [[nodiscard]] uint32_t width() const { return info_.width; }
        [[nodiscard]] uint32_t height() const { return info_.height; }
        [[nodiscard]] BitDepth bit_depth() const { return info_.bit_depth; }
        [[nodiscard]] bool has_alpha() const { return info_.has_alpha; }
        [[nodiscard]] const ImageInfo &info() const { return info_; }

        [[nodiscard]] std::span<uint8_t> pixels() { return pixels_; }
        [[nodiscard]] std::span<const uint8_t> pixels() const { return pixels_; }

        [[nodiscard]] uint8_t *row(uint32_t y);
        [[nodiscard]] const uint8_t *row(uint32_t y) const;

        [[nodiscard]] uint32_t bytes_per_pixel() const;

        [[nodiscard]] size_t stride() const;

    private:
        ImageInfo info_;
        std::vector<uint8_t> pixels_;
    };

    struct EncoderConfig
    {
        float quality = 75.0f;
        bool lossless = false;
        BitDepth bit_depth = BitDepth::Bits8;
        Subsampling subsampling = Subsampling::YUV444;
        CICP cicp = {};
        uint8_t tile_size_log2 = 0;
        uint32_t threads = 0;
        // Reserved: accepted but currently ignored -- no
        // SSIMULACRA2-targeted mode exists yet (the wkenc flag was
        // removed rather than advertise a no-op).
        float target_ssimulacra2 = 0.0f;
    };

    [[nodiscard]] Result<std::vector<uint8_t>> encode(const Image &image, const EncoderConfig &config = {});

    struct DecoderConfig
    {
        uint32_t threads = 0;
        bool info_only = false;
        bool dc_only = false;
    };

    [[nodiscard]] Result<ImageInfo> get_info(std::span<const uint8_t> data);

    [[nodiscard]] Result<Image> decode(std::span<const uint8_t> data, const DecoderConfig &config = {});

    constexpr std::string_view version() { return "0.2.1"; }

}
