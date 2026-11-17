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

#include "image_io.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

namespace wk::io {

namespace {

std::string lowercase_extension(std::string_view path) {
    std::filesystem::path fs_path{std::string(path)};
    std::string ext = fs_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

Result<std::vector<uint8_t>> read_file_bytes(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to open file"});
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to determine file size"});
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to read file"});
    }
    return bytes;
}

Result<std::string> next_ppm_token(std::span<const uint8_t> data, size_t& offset) {
    while (offset < data.size()) {
        const unsigned char ch = static_cast<unsigned char>(data[offset]);
        if (std::isspace(ch)) {
            ++offset;
            continue;
        }
        if (ch == '#') {
            while (offset < data.size() && data[offset] != '\n') {
                ++offset;
            }
            continue;
        }
        break;
    }

    if (offset >= data.size()) {
        return std::unexpected(Error{ErrorCode::TruncatedInput, "unexpected end of PPM header"});
    }

    const size_t start = offset;
    while (offset < data.size() && !std::isspace(static_cast<unsigned char>(data[offset]))) {
        ++offset;
    }
    return std::string(reinterpret_cast<const char*>(data.data() + start), offset - start);
}

// PPM header numbers come from the file, so they must be parsed without
// throwing: std::stoul on a 26-digit width threw std::out_of_range out of
// load_ppm with no handler (wkenc aborted, exit 134), and "-5" wrapped to
// ULONG_MAX and was only caught later by the payload-size check.
Result<uint32_t> parse_ppm_u32(const std::string& token) {
    if (token.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "empty PPM header value"});
    }
    uint64_t value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "PPM header value is not a number"});
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > UINT32_MAX) {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "PPM header value out of range"});
        }
    }
    return static_cast<uint32_t>(value);
}

Result<Image> load_ppm(std::string_view path) {
    auto file_bytes = read_file_bytes(path);
    if (!file_bytes) {
        return std::unexpected(file_bytes.error());
    }

    size_t offset = 0;
    auto magic = next_ppm_token(*file_bytes, offset);
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (*magic != "P6") {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "only binary P6 PPM is supported"});
    }

    auto width_token = next_ppm_token(*file_bytes, offset);
    auto height_token = next_ppm_token(*file_bytes, offset);
    auto max_token = next_ppm_token(*file_bytes, offset);
    if (!width_token || !height_token || !max_token) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "incomplete PPM header"});
    }

    auto width = parse_ppm_u32(*width_token);
    if (!width) {
        return std::unexpected(width.error());
    }
    auto height = parse_ppm_u32(*height_token);
    if (!height) {
        return std::unexpected(height.error());
    }
    auto max_value = parse_ppm_u32(*max_token);
    if (!max_value) {
        return std::unexpected(max_value.error());
    }
    if (*width == 0 || *height == 0) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "PPM dimensions must be non-zero"});
    }
    // Same caps as the container and the stb path (65535 per side,
    // 2^28 pixels): without them a huge width/height overflows the
    // payload-size arithmetic below.
    if (*width > 65535 || *height > 65535 ||
        static_cast<uint64_t>(*width) * static_cast<uint64_t>(*height) > (1u << 28)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "PPM dimensions out of range"});
    }
    // 16-bit PPM (maxval 1023/4095) is the HDR input path: the
    // raster is big-endian uint16 per the PPM spec, stored
    // native-endian in the Image like every other 16-bit source.
    BitDepth bit_depth;
    if (*max_value == 255) {
        bit_depth = BitDepth::Bits8;
    } else if (*max_value == 1023) {
        bit_depth = BitDepth::Bits10;
    } else if (*max_value == 4095) {
        bit_depth = BitDepth::Bits12;
    } else {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature,
                                     "PPM max value must be 255, 1023 or 4095"});
    }

    // The raster follows a single whitespace delimiter after maxval.
    // Skip exactly one byte (a CRLF pair counts as one line ending).
    // Skipping all whitespace here is wrong: a leading raster byte can
    // itself be a whitespace value (e.g. 0x09), and eating it shifts the
    // raster and trips the payload-size check below.
    if (offset < file_bytes->size()) {
        if ((*file_bytes)[offset] == '\r' && offset + 1 < file_bytes->size() &&
            (*file_bytes)[offset + 1] == '\n') {
            offset += 2;
        } else if (std::isspace(static_cast<unsigned char>((*file_bytes)[offset]))) {
            ++offset;
        }
    }

    const size_t bytes_per_sample = (bit_depth == BitDepth::Bits8) ? 1 : 2;
    const size_t rgb_size = static_cast<size_t>(*width) * *height * 3 * bytes_per_sample;
    if (file_bytes->size() - offset != rgb_size) {
        return std::unexpected(Error{ErrorCode::InvalidChunkSize, "PPM payload size does not match dimensions"});
    }

    Image image(*width, *height, bit_depth, false);
    if (bit_depth == BitDepth::Bits8) {
        std::copy(file_bytes->begin() + static_cast<std::ptrdiff_t>(offset), file_bytes->end(), image.pixels().begin());
        return image;
    }
    // Big-endian raster -> native-endian uint16 samples.
    uint16_t* out = reinterpret_cast<uint16_t*>(image.pixels().data());
    const uint8_t* src = file_bytes->data() + offset;
    for (size_t i = 0; i < static_cast<size_t>(*width) * *height * 3; ++i) {
        out[i] = static_cast<uint16_t>((static_cast<uint16_t>(src[2 * i]) << 8) |
                                       static_cast<uint16_t>(src[2 * i + 1]));
    }
    return image;
}

Result<void> save_ppm(std::string_view path, const Image& image) {
    if (image.bit_depth() != BitDepth::Bits8 && image.bit_depth() != BitDepth::Bits10 &&
        image.bit_depth() != BitDepth::Bits12) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature, "PPM output only supports 8/10/12-bit images"});
    }

    std::ofstream file(std::string(path), std::ios::binary);
    if (!file) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to open output file"});
    }

    const uint16_t maxval = static_cast<uint16_t>((1u << static_cast<unsigned>(image.bit_depth())) - 1u);
    file << "P6\n" << image.width() << ' ' << image.height() << "\n" << maxval << "\n";
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    const auto pixels = image.pixels();

    if (image.bit_depth() == BitDepth::Bits8) {
        if (image.has_alpha()) {
            for (size_t i = 0; i < pixel_count; ++i) {
                file.put(static_cast<char>(pixels[i * 4 + 0]));
                file.put(static_cast<char>(pixels[i * 4 + 1]));
                file.put(static_cast<char>(pixels[i * 4 + 2]));
            }
        } else {
            file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        }
    } else {
        // Big-endian uint16 raster per the PPM spec; alpha is dropped
        // like on the 8-bit path (PPM has no alpha channel).
        const uint16_t* samples = reinterpret_cast<const uint16_t*>(pixels.data());
        const size_t stride = image.has_alpha() ? 4 : 3;
        for (size_t i = 0; i < pixel_count; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                const uint16_t v = samples[i * stride + c];
                file.put(static_cast<char>(v >> 8));
                file.put(static_cast<char>(v & 0xff));
            }
        }
    }
    // ofstream buffers silently: a full disk or broken pipe only shows
    // up when the stream is flushed, so check before reporting success.
    file.flush();
    if (!file) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to write output file"});
    }
    return {};
}

// stb_image decodes JPG/PNG/BMP/TGA/GIF (and more) on every platform,
// replacing the old Windows-only WIC path: wkenc could only read PPM
// on Linux/Termux, and wkdec could only write PNG on Windows.
// Dimensions are probed with stbi_info first so a file claiming a huge
// size is rejected before any raster allocation (same caps as the
// container: 65535 per side, 2^28 pixels).
Result<Image> load_stb_image(std::string_view path) {
    const std::string file_path(path);
    int width = 0, height = 0, channels = 0;
    if (stbi_info(file_path.c_str(), &width, &height, &channels) == 0) {
        return std::unexpected(Error{ErrorCode::IoError,
                                     std::string("failed to read image: ") + stbi_failure_reason()});
    }
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535 ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > (1u << 28)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "image dimensions out of range"});
    }
    if (channels != 1 && channels != 2 && channels != 3 && channels != 4) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature, "unsupported channel count"});
    }

    unsigned char* data = stbi_load(file_path.c_str(), &width, &height, &channels, 0);
    if (!data) {
        return std::unexpected(Error{ErrorCode::IoError,
                                     std::string("failed to load image: ") + stbi_failure_reason()});
    }

    const bool has_alpha = (channels == 2 || channels == 4);
    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height), BitDepth::Bits8,
                has_alpha);
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    auto out = image.pixels();
    switch (channels) {
        case 1:
            for (size_t i = 0; i < pixel_count; ++i) {
                out[i * 3 + 0] = data[i];
                out[i * 3 + 1] = data[i];
                out[i * 3 + 2] = data[i];
            }
            break;
        case 2:
            for (size_t i = 0; i < pixel_count; ++i) {
                out[i * 4 + 0] = data[i * 2 + 0];
                out[i * 4 + 1] = data[i * 2 + 0];
                out[i * 4 + 2] = data[i * 2 + 0];
                out[i * 4 + 3] = data[i * 2 + 1];
            }
            break;
        case 3:
        case 4:
            std::copy(data, data + pixel_count * static_cast<size_t>(channels), out.begin());
            break;
    }
    stbi_image_free(data);
    return image;
}

Result<void> save_stb_png(std::string_view path, const Image& image) {
    if (image.bit_depth() != BitDepth::Bits8) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature, "PNG output only supports 8-bit images"});
    }
    const int channels = image.has_alpha() ? 4 : 3;
    const int stride = static_cast<int>(image.width()) * channels;
    const int ok = stbi_write_png(std::string(path).c_str(), static_cast<int>(image.width()),
                                  static_cast<int>(image.height()), channels,
                                  image.pixels().data(), stride);
    if (ok == 0) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to write PNG file"});
    }
    return {};
}

}

Result<Image> load_image_file(std::string_view path) {
    const std::string ext = lowercase_extension(path);
    if (ext == ".ppm") {
        return load_ppm(path);
    }
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
        ext == ".tga" || ext == ".gif") {
        return load_stb_image(path);
    }
    return std::unexpected(Error{ErrorCode::UnsupportedFeature, "unsupported input image format"});
}

Result<void> save_image_file(std::string_view path, const Image& image) {
    const std::string ext = lowercase_extension(path);
    if (ext == ".ppm") {
        return save_ppm(path, image);
    }
    if (ext == ".png") {
        return save_stb_png(path, image);
    }
    return std::unexpected(Error{ErrorCode::UnsupportedFeature, "unsupported output image format"});
}

}
