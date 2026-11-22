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

#include "meta_compress.h"
#include <zlib.h>

namespace wk::meta
{

namespace
{

// One inflate output chunk: small enough to keep the bomb bound tight,
// large enough that the loop rarely iterates on real payloads.
constexpr size_t kInflateChunk = 8192;

}

std::optional<std::vector<uint8_t>>
compress_meta_payload(std::span<const uint8_t> raw)
{
    if (raw.empty())
    {
        return std::nullopt;
    }
    std::vector<uint8_t> out(compressBound(static_cast<uLong>(raw.size())));
    uLongf out_len = static_cast<uLongf>(out.size());
    const int rc = compress2(out.data(), &out_len, raw.data(),
                             static_cast<uLong>(raw.size()), 6);
    if (rc != Z_OK)
    {
        return std::nullopt;
    }
    out.resize(out_len);
    if (out.size() >= raw.size())
    {
        return std::nullopt;
    }
    return out;
}

Result<std::vector<uint8_t>>
decompress_meta_payload(std::span<const uint8_t> compressed)
{
    if (compressed.empty())
    {
        return std::unexpected(Error{ErrorCode::MetaParseError,
                                     "empty compressed META payload"});
    }
    z_stream strm{};
    strm.next_in = const_cast<Bytef *>(compressed.data());
    strm.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit(&strm) != Z_OK)
    {
        return std::unexpected(Error{ErrorCode::MetaParseError,
                                     "zlib init failed"});
    }

    std::vector<uint8_t> out;
    std::vector<uint8_t> chunk(kInflateChunk);
    int rc = Z_OK;
    while (rc != Z_STREAM_END)
    {
        if (out.size() > kMaxDecompressedMetaPayload)
        {
            inflateEnd(&strm);
            return std::unexpected(Error{ErrorCode::MetaParseError,
                                         "decompressed META payload exceeds limit"});
        }
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
        {
            inflateEnd(&strm);
            return std::unexpected(Error{ErrorCode::MetaParseError,
                                         "invalid compressed META payload"});
        }
        const size_t produced = chunk.size() - strm.avail_out;
        if (produced == 0 && rc == Z_BUF_ERROR)
        {
            // No progress and no more input: truncated stream.
            inflateEnd(&strm);
            return std::unexpected(Error{ErrorCode::MetaParseError,
                                         "truncated compressed META payload"});
        }
        out.insert(out.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(produced));
        if (out.size() > kMaxDecompressedMetaPayload)
        {
            inflateEnd(&strm);
            return std::unexpected(Error{ErrorCode::MetaParseError,
                                         "decompressed META payload exceeds limit"});
        }
    }
    inflateEnd(&strm);
    if (strm.avail_in != 0)
    {
        return std::unexpected(Error{ErrorCode::MetaParseError,
                                     "trailing data after compressed META payload"});
    }
    return out;
}

} // namespace wk::meta
