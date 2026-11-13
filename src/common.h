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

#include <wk/wk.hpp>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <variant>
#include <algorithm>
#include <bit>
#include <concepts>
#include <cassert>

namespace wk
{

    // ErrorCode, Error, and Result<T> live in the public <wk/wk.hpp>
    // (see the note there) so the public header is self-contained.
    // This file adds the internal helpers shared by the implementation.

    inline uint16_t read_le16(const uint8_t *p)
    {
        uint16_t v;
        std::memcpy(&v, p, 2);
        if constexpr (std::endian::native == std::endian::big)
        {
            v = __builtin_bswap16(v);
        }
        return v;
    }

    inline uint32_t read_le32(const uint8_t *p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        if constexpr (std::endian::native == std::endian::big)
        {
            v = __builtin_bswap32(v);
        }
        return v;
    }

    inline uint64_t read_le64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        if constexpr (std::endian::native == std::endian::big)
        {
            v = __builtin_bswap64(v);
        }
        return v;
    }

    inline float read_le_f32(const uint8_t *p)
    {
        uint32_t bits = read_le32(p);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    inline double read_le_f64(const uint8_t *p)
    {
        uint64_t bits = read_le64(p);
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    inline void write_le16(uint8_t *p, uint16_t v)
    {
        if constexpr (std::endian::native == std::endian::big)
        {
            v = __builtin_bswap16(v);
        }
        std::memcpy(p, &v, 2);
    }

    inline void write_le32(uint8_t *p, uint32_t v)
    {
        if constexpr (std::endian::native == std::endian::big)
        {
            v = __builtin_bswap32(v);
        }
        std::memcpy(p, &v, 4);
    }

    inline void write_le64(uint8_t *p, uint64_t v)
    {
        if constexpr (std::endian::native == std::endian::big)
        {
            v = __builtin_bswap64(v);
        }
        std::memcpy(p, &v, 8);
    }

    inline void write_le_f32(uint8_t *p, float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        write_le32(p, bits);
    }

    inline void write_le_f64(uint8_t *p, double v)
    {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        write_le64(p, bits);
    }

    class ByteWriter
    {
    public:
        void write_u8(uint8_t v) { data_.push_back(v); }
        void write_u16(uint16_t v)
        {
            uint8_t buf[2];
            write_le16(buf, v);
            data_.insert(data_.end(), buf, buf + 2);
        }
        void write_u32(uint32_t v)
        {
            uint8_t buf[4];
            write_le32(buf, v);
            data_.insert(data_.end(), buf, buf + 4);
        }
        void write_u64(uint64_t v)
        {
            uint8_t buf[8];
            write_le64(buf, v);
            data_.insert(data_.end(), buf, buf + 8);
        }
        void write_i32(int32_t v)
        {
            write_u32(static_cast<uint32_t>(v));
        }
        void write_f32(float v)
        {
            uint8_t buf[4];
            write_le_f32(buf, v);
            data_.insert(data_.end(), buf, buf + 4);
        }
        void write_f64(double v)
        {
            uint8_t buf[8];
            write_le_f64(buf, v);
            data_.insert(data_.end(), buf, buf + 8);
        }
        void write_bytes(std::span<const uint8_t> bytes)
        {
            data_.insert(data_.end(), bytes.begin(), bytes.end());
        }
        void write_str(std::string_view s)
        {
            data_.insert(data_.end(),
                         reinterpret_cast<const uint8_t *>(s.data()),
                         reinterpret_cast<const uint8_t *>(s.data() + s.size()));
        }
        void write_at_u32(size_t offset, uint32_t v)
        {
            assert(offset + 4 <= data_.size());
            write_le32(data_.data() + offset, v);
        }

        [[nodiscard]] size_t size() const { return data_.size(); }
        [[nodiscard]] const uint8_t *data() const { return data_.data(); }
        [[nodiscard]] std::vector<uint8_t> finish() { return std::move(data_); }
        void reserve(size_t n) { data_.reserve(n); }

    private:
        std::vector<uint8_t> data_;
    };

    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const uint8_t> data)
            : data_(data), pos_(0) {}

        [[nodiscard]] size_t remaining() const { return data_.size() - pos_; }
        [[nodiscard]] size_t position() const { return pos_; }
        [[nodiscard]] size_t size() const { return data_.size(); }
        [[nodiscard]] bool at_end() const { return pos_ >= data_.size(); }

        // Reposition the reader. Used by the WKMETA parser to move past an
        // entry whose declared size is authoritative for framing (see
        // MetaBlock::parse). The target is clamped to the buffer end so a
        // seek can never run the reader out of bounds.
        void seek(size_t pos) { pos_ = std::min(pos, data_.size()); }

        [[nodiscard]] Result<uint8_t> read_u8()
        {
            if (remaining() < 1)
                return std::unexpected(Error{ErrorCode::TruncatedInput, "need 1 byte"});
            return data_[pos_++];
        }
        [[nodiscard]] Result<uint16_t> read_u16()
        {
            if (remaining() < 2)
                return std::unexpected(Error{ErrorCode::TruncatedInput, "need 2 bytes"});
            auto v = read_le16(data_.data() + pos_);
            pos_ += 2;
            return v;
        }
        [[nodiscard]] Result<uint32_t> read_u32()
        {
            if (remaining() < 4)
                return std::unexpected(Error{ErrorCode::TruncatedInput, "need 4 bytes"});
            auto v = read_le32(data_.data() + pos_);
            pos_ += 4;
            return v;
        }
        [[nodiscard]] Result<uint64_t> read_u64()
        {
            if (remaining() < 8)
                return std::unexpected(Error{ErrorCode::TruncatedInput, "need 8 bytes"});
            auto v = read_le64(data_.data() + pos_);
            pos_ += 8;
            return v;
        }
        [[nodiscard]] Result<int32_t> read_i32()
        {
            auto r = read_u32();
            if (!r)
                return std::unexpected(r.error());
            return static_cast<int32_t>(*r);
        }
        [[nodiscard]] Result<float> read_f32()
        {
            if (remaining() < 4)
                return std::unexpected(Error{ErrorCode::TruncatedInput, "need 4 bytes"});
            auto v = read_le_f32(data_.data() + pos_);
            pos_ += 4;
            return v;
        }
        [[nodiscard]] Result<double> read_f64()
        {
            if (remaining() < 8)
                return std::unexpected(Error{ErrorCode::TruncatedInput, "need 8 bytes"});
            auto v = read_le_f64(data_.data() + pos_);
            pos_ += 8;
            return v;
        }
        [[nodiscard]] Result<std::span<const uint8_t>> read_bytes(size_t n)
        {
            if (remaining() < n)
                return std::unexpected(Error{ErrorCode::TruncatedInput});
            auto span = data_.subspan(pos_, n);
            pos_ += n;
            return span;
        }
        [[nodiscard]] Result<std::string> read_str(size_t n)
        {
            auto r = read_bytes(n);
            if (!r)
                return std::unexpected(r.error());
            return std::string(reinterpret_cast<const char *>(r->data()), r->size());
        }

        void skip(size_t n) { pos_ = std::min(pos_ + n, data_.size()); }

    private:
        std::span<const uint8_t> data_;
        size_t pos_;
    };

    enum class PixelFormat : uint8_t
    {
        RGBA8 = 0,
        RGB8 = 1,
        RGBA16 = 2,
        RGB16 = 3,
        Y8 = 4,
        Y16 = 5,
        YA8 = 6,
        YA16 = 7,
    };

    inline uint8_t channels_for_format(PixelFormat fmt)
    {
        switch (fmt)
        {
        case PixelFormat::RGBA8:
            return 4;
        case PixelFormat::RGB8:
            return 3;
        case PixelFormat::RGBA16:
            return 4;
        case PixelFormat::RGB16:
            return 3;
        case PixelFormat::Y8:
            return 1;
        case PixelFormat::Y16:
            return 1;
        case PixelFormat::YA8:
            return 2;
        case PixelFormat::YA16:
            return 2;
        }
        return 0;
    }

    inline uint8_t bytes_per_pixel(PixelFormat fmt)
    {
        uint8_t ch = channels_for_format(fmt);
        bool is16 = (fmt == PixelFormat::RGBA16 || fmt == PixelFormat::RGB16 ||
                     fmt == PixelFormat::Y16 || fmt == PixelFormat::YA16);
        return ch * (is16 ? 2 : 1);
    }

    struct ImageBuffer
    {
        uint32_t width = 0;
        uint32_t height = 0;
        PixelFormat format = PixelFormat::RGBA8;
        std::vector<uint8_t> data;

        [[nodiscard]] size_t stride() const
        {
            return static_cast<size_t>(width) * bytes_per_pixel(format);
        }
        [[nodiscard]] size_t total_bytes() const
        {
            return stride() * height;
        }
        void allocate()
        {
            data.resize(total_bytes());
        }
        [[nodiscard]] uint8_t *row(uint32_t y)
        {
            return data.data() + static_cast<size_t>(y) * stride();
        }
        [[nodiscard]] const uint8_t *row(uint32_t y) const
        {
            return data.data() + static_cast<size_t>(y) * stride();
        }
    };

    enum class ChromaSubsampling : uint8_t
    {
        YUV444 = 0,
        YUV420 = 1,
    };

    constexpr uint8_t WK_MAGIC[5] = {0x57, 0x4B, 0x49, 0x4D, 0x47};
    constexpr uint16_t WK_VERSION = 0x0001;

    constexpr char CHUNK_FHDR[4] = {'F', 'H', 'D', 'R'};
    constexpr char CHUNK_WKMETA[4] = {'M', 'E', 'T', 'A'};
    constexpr char CHUNK_ICCP[4] = {'I', 'C', 'C', 'P'};
    constexpr char CHUNK_PROV[4] = {'P', 'R', 'O', 'V'};
    constexpr char CHUNK_ANIM[4] = {'A', 'N', 'I', 'M'};
    constexpr char CHUNK_TILE[4] = {'T', 'I', 'L', 'E'};
    constexpr char CHUNK_FEND[4] = {'F', 'E', 'N', 'D'};

    constexpr uint8_t CHUNK_FLAG_OPTIONAL = 0x01;
    constexpr uint8_t CHUNK_FLAG_META_ONLY = 0x02;
    constexpr uint8_t CHUNK_FLAG_REPEATABLE = 0x04;

    constexpr uint16_t FHDR_FLAG_LOSSLESS = 0x0001;
    constexpr uint16_t FHDR_FLAG_ANIMATED = 0x0002;
    constexpr uint16_t FHDR_FLAG_ALPHA = 0x0004;
    constexpr uint16_t FHDR_FLAG_HDR = 0x0008;
    constexpr uint16_t FHDR_FLAG_TILED = 0x0010;
    constexpr uint16_t FHDR_FLAG_HAS_WKMETA = 0x0020;
    constexpr uint16_t FHDR_FLAG_HAS_C2PA = 0x0040;
    constexpr uint16_t FHDR_FLAG_FULL_RANGE = 0x0080;

    struct FrameHeader
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint8_t bit_depth = 8;
        uint8_t cicp_primaries = 1;
        uint8_t cicp_transfer = 1;
        uint8_t cicp_matrix = 1;
        uint16_t flags = 0;
        uint8_t tile_size_log2 = 9;
        uint16_t max_cll = 0;
        uint16_t max_fall = 0;

        [[nodiscard]] uint32_t tile_size() const { return 1u << tile_size_log2; }
        [[nodiscard]] bool is_lossless() const { return flags & FHDR_FLAG_LOSSLESS; }
        [[nodiscard]] bool is_animated() const { return flags & FHDR_FLAG_ANIMATED; }
        [[nodiscard]] bool has_alpha() const { return flags & FHDR_FLAG_ALPHA; }
        [[nodiscard]] bool is_hdr() const { return flags & FHDR_FLAG_HDR; }
        [[nodiscard]] bool is_tiled() const { return flags & FHDR_FLAG_TILED; }
        [[nodiscard]] bool has_wkmeta() const { return flags & FHDR_FLAG_HAS_WKMETA; }
        [[nodiscard]] bool full_range() const { return flags & FHDR_FLAG_FULL_RANGE; }

        [[nodiscard]] uint32_t tiles_x() const
        {
            return (width + tile_size() - 1) / tile_size();
        }
        [[nodiscard]] uint32_t tiles_y() const
        {
            return (height + tile_size() - 1) / tile_size();
        }
        [[nodiscard]] uint32_t tile_count() const
        {
            // The product is formed in 64-bit: the parse_fhdr caps
            // (65535 px per side, 2^28 pixels) bound it far below
            // 2^32, but the multiply itself must not be the thing
            // that overflows first if those caps ever change.
            return static_cast<uint32_t>(static_cast<uint64_t>(tiles_x()) * tiles_y());
        }
        [[nodiscard]] uint16_t max_sample_value() const
        {
            switch (bit_depth)
            {
            case 10:
                return 1023;
            case 12:
                return 4095;
            default:
                return 255;
            }
        }
    };

    struct TileHeader
    {
        uint16_t tile_x = 0;
        uint16_t tile_y = 0;
        uint8_t layer_flags = 0;
        uint32_t compressed_size = 0;
    };

    constexpr uint8_t TILE_HAS_BASE = 0x01;
    constexpr uint8_t TILE_HAS_REFINEMENT = 0x02;
    constexpr uint8_t TILE_HAS_ALPHA = 0x04;

    struct AnimFrame
    {
        uint16_t delay_ms = 0;
        uint8_t blend_mode = 0;
        uint8_t disposal = 0;
        uint16_t rect_x = 0;
        uint16_t rect_y = 0;
        uint16_t rect_w = 0;
        uint16_t rect_h = 0;
        uint32_t tile_offset = 0;
    };

    struct AnimHeader
    {
        uint32_t frame_count = 0;
        uint16_t loop_count = 0;
        uint32_t background_rgba = 0;
        std::vector<AnimFrame> frames;
    };

}
