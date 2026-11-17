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

#include "webp_decode.h"
#include "src/webp/decode.h"
#include <cstring>

namespace wk::webp {

bool has_webp_magic(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12) {
        return false;
    }
    return std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
           std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

Result<Image> decode(std::span<const uint8_t> bytes) {
    if (!has_webp_magic(bytes)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "not a WebP file"});
    }

    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "invalid WebP header"});
    }
    // Same caps as the container and the stb path (65535 per side,
    // 2^28 pixels): probe dimensions before any raster allocation so
    // a file claiming a huge size is rejected up front.
    if (features.width <= 0 || features.height <= 0 || features.width > 65535 ||
        features.height > 65535 ||
        static_cast<uint64_t>(features.width) * static_cast<uint64_t>(features.height) >
            (1u << 28)) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "WebP dimensions out of range"});
    }

    const bool has_alpha = features.has_alpha != 0;
    int width = 0, height = 0;
    uint8_t* decoded = has_alpha
                           ? WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height)
                           : WebPDecodeRGB(bytes.data(), bytes.size(), &width, &height);
    if (!decoded) {
        return std::unexpected(Error{ErrorCode::InvalidHeader, "failed to decode WebP"});
    }

    Image image(static_cast<uint32_t>(width), static_cast<uint32_t>(height), BitDepth::Bits8,
                has_alpha);
    auto out = image.pixels();
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::copy(decoded, decoded + pixel_count * (has_alpha ? 4u : 3u), out.begin());
    WebPFree(decoded);
    return image;
}

}  // namespace wk::webp
