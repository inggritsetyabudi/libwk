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
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#include <windows.h>
#include <wincodec.h>
#endif

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
    if (*max_value != 255) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature, "PPM max value must be 255"});
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

    const size_t rgb_size = static_cast<size_t>(*width) * *height * 3;
    if (file_bytes->size() - offset != rgb_size) {
        return std::unexpected(Error{ErrorCode::InvalidChunkSize, "PPM payload size does not match dimensions"});
    }

    Image image(*width, *height, BitDepth::Bits8, false);
    std::copy(file_bytes->begin() + static_cast<std::ptrdiff_t>(offset), file_bytes->end(), image.pixels().begin());
    return image;
}

Result<void> save_ppm(std::string_view path, const Image& image) {
    if (image.bit_depth() != BitDepth::Bits8) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature, "PPM output only supports 8-bit images"});
    }

    std::ofstream file(std::string(path), std::ios::binary);
    if (!file) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to open output file"});
    }

    file << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    const auto pixels = image.pixels();

    if (image.has_alpha()) {
        for (size_t i = 0; i < pixel_count; ++i) {
            file.put(static_cast<char>(pixels[i * 4 + 0]));
            file.put(static_cast<char>(pixels[i * 4 + 1]));
            file.put(static_cast<char>(pixels[i * 4 + 2]));
        }
    } else {
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }
    // ofstream buffers silently: a full disk or broken pipe only shows
    // up when the stream is flushed, so check before reporting success.
    file.flush();
    if (!file) {
        return std::unexpected(Error{ErrorCode::IoError, "failed to write output file"});
    }
    return {};
}

#ifdef _WIN32

struct ComReleaser {
    template <typename T>
    void operator()(T* ptr) const {
        if (ptr) {
            ptr->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaser>;

class ScopedComInit {
public:
    ScopedComInit() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    ~ScopedComInit() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool ok() const {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT hr() const {
        return hr_;
    }

private:
    HRESULT hr_ = E_FAIL;
};

std::wstring utf8_to_wide(std::string_view input) {
    if (input.empty()) {
        return {};
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    if (length == 0) {
        codepage = CP_ACP;
        length = MultiByteToWideChar(codepage, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    }
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codepage, 0, input.data(), static_cast<int>(input.size()), output.data(), length);
    return output;
}

Error wic_error(HRESULT hr, std::string message) {
    std::ostringstream builder;
    builder << std::move(message) << " (HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << ')';
    return Error{ErrorCode::IoError, builder.str()};
}

bool format_supports_transparency(IWICImagingFactory* factory, REFWICPixelFormatGUID format) {
    IWICComponentInfo* raw_component = nullptr;
    if (FAILED(factory->CreateComponentInfo(format, &raw_component))) {
        return false;
    }
    ComPtr<IWICComponentInfo> component(raw_component);

    IWICPixelFormatInfo2* raw_info = nullptr;
    if (FAILED(component->QueryInterface(IID_IWICPixelFormatInfo2, reinterpret_cast<void**>(&raw_info)))) {
        return false;
    }
    ComPtr<IWICPixelFormatInfo2> info(raw_info);

    BOOL supports = FALSE;
    if (FAILED(info->SupportsTransparency(&supports))) {
        return false;
    }
    return supports != FALSE;
}

Result<Image> load_wic_image(std::string_view path) {
    ScopedComInit com;
    if (!com.ok()) {
        return std::unexpected(wic_error(com.hr(), "failed to initialize COM"));
    }

    IWICImagingFactory* raw_factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, reinterpret_cast<void**>(&raw_factory));
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to create WIC factory"));
    }
    ComPtr<IWICImagingFactory> factory(raw_factory);

    IWICBitmapDecoder* raw_decoder = nullptr;
    const std::wstring wide_path = utf8_to_wide(path);
    hr = factory->CreateDecoderFromFilename(wide_path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &raw_decoder);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to open image file"));
    }
    ComPtr<IWICBitmapDecoder> decoder(raw_decoder);

    IWICBitmapFrameDecode* raw_frame = nullptr;
    hr = decoder->GetFrame(0, &raw_frame);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to read image frame"));
    }
    ComPtr<IWICBitmapFrameDecode> frame(raw_frame);

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to read image dimensions"));
    }

    WICPixelFormatGUID source_format = {};
    hr = frame->GetPixelFormat(&source_format);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to read source pixel format"));
    }
    const bool has_alpha = format_supports_transparency(factory.get(), source_format);

    IWICFormatConverter* raw_converter = nullptr;
    hr = factory->CreateFormatConverter(&raw_converter);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to create format converter"));
    }
    ComPtr<IWICFormatConverter> converter(raw_converter);

    const WICPixelFormatGUID target_format = has_alpha ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat24bppRGB;
    hr = converter->Initialize(frame.get(), target_format,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to convert image to an 8-bit RGB format"));
    }

    Image image(width, height, BitDepth::Bits8, has_alpha);
    const UINT stride = width * image.bytes_per_pixel();
    hr = converter->CopyPixels(nullptr, stride,
                               static_cast<UINT>(image.pixels().size()),
                               image.pixels().data());
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to copy image pixels"));
    }

    return image;
}

Result<void> save_png(std::string_view path, const Image& image) {
    if (image.bit_depth() != BitDepth::Bits8) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature, "PNG output only supports 8-bit images"});
    }

    ScopedComInit com;
    if (!com.ok()) {
        return std::unexpected(wic_error(com.hr(), "failed to initialize COM"));
    }

    IWICImagingFactory* raw_factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, reinterpret_cast<void**>(&raw_factory));
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to create WIC factory"));
    }
    ComPtr<IWICImagingFactory> factory(raw_factory);

    IWICStream* raw_stream = nullptr;
    hr = factory->CreateStream(&raw_stream);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to create WIC stream"));
    }
    ComPtr<IWICStream> stream(raw_stream);

    const std::wstring wide_path = utf8_to_wide(path);
    hr = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to open PNG output"));
    }

    IWICBitmapEncoder* raw_encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &raw_encoder);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to create PNG encoder"));
    }
    ComPtr<IWICBitmapEncoder> encoder(raw_encoder);

    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to initialize PNG encoder"));
    }

    IWICBitmapFrameEncode* raw_frame = nullptr;
    IPropertyBag2* raw_props = nullptr;
    hr = encoder->CreateNewFrame(&raw_frame, &raw_props);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to create PNG frame"));
    }
    ComPtr<IWICBitmapFrameEncode> frame(raw_frame);
    ComPtr<IPropertyBag2> props(raw_props);

    hr = frame->Initialize(props.get());
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to initialize PNG frame"));
    }

    hr = frame->SetSize(image.width(), image.height());
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to set PNG size"));
    }

    std::vector<uint8_t> encoded_pixels;
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();
    const auto pixels = image.pixels();
    UINT stride = 0;
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;

    if (image.has_alpha()) {
        pixel_format = GUID_WICPixelFormat32bppBGRA;
        stride = image.width() * 4;
        encoded_pixels.resize(pixel_count * 4);
        for (size_t i = 0; i < pixel_count; ++i) {
            encoded_pixels[i * 4 + 0] = pixels[i * 4 + 2];
            encoded_pixels[i * 4 + 1] = pixels[i * 4 + 1];
            encoded_pixels[i * 4 + 2] = pixels[i * 4 + 0];
            encoded_pixels[i * 4 + 3] = pixels[i * 4 + 3];
        }
    } else {
        pixel_format = GUID_WICPixelFormat24bppBGR;
        stride = image.width() * 3;
        encoded_pixels.resize(pixel_count * 3);
        for (size_t i = 0; i < pixel_count; ++i) {
            encoded_pixels[i * 3 + 0] = pixels[i * 3 + 2];
            encoded_pixels[i * 3 + 1] = pixels[i * 3 + 1];
            encoded_pixels[i * 3 + 2] = pixels[i * 3 + 0];
        }
    }

    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to set PNG pixel format"));
    }

    hr = frame->WritePixels(image.height(), stride,
                            static_cast<UINT>(encoded_pixels.size()),
                            encoded_pixels.data());
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to write PNG pixels"));
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to finalize PNG frame"));
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        return std::unexpected(wic_error(hr, "failed to finalize PNG file"));
    }

    return {};
}

#endif

}

Result<Image> load_image_file(std::string_view path) {
    const std::string ext = lowercase_extension(path);
    if (ext == ".ppm") {
        return load_ppm(path);
    }
#ifdef _WIN32
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
        return load_wic_image(path);
    }
#endif
    return std::unexpected(Error{ErrorCode::UnsupportedFeature, "unsupported input image format"});
}

Result<void> save_image_file(std::string_view path, const Image& image) {
    const std::string ext = lowercase_extension(path);
    if (ext == ".ppm") {
        return save_ppm(path, image);
    }
#ifdef _WIN32
    if (ext == ".png") {
        return save_png(path, image);
    }
#endif
    return std::unexpected(Error{ErrorCode::UnsupportedFeature, "unsupported output image format"});
}

}
