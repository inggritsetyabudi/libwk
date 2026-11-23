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

#include "container.h"
#include "meta_compress.h"
#include <cstring>

namespace wk
{

    std::vector<uint8_t> serialize_fhdr(const FrameHeader &hdr)
    {
        ByteWriter w;
        w.reserve(20);
        w.write_u32(hdr.width);
        w.write_u32(hdr.height);
        w.write_u8(hdr.bit_depth);
        w.write_u8(hdr.cicp_primaries);
        w.write_u8(hdr.cicp_transfer);
        w.write_u8(hdr.cicp_matrix);
        w.write_u16(hdr.flags);
        w.write_u8(hdr.tile_size_log2);
        w.write_u16(hdr.max_cll);
        w.write_u16(hdr.max_fall);
        return w.finish();
    }

    Result<FrameHeader> parse_fhdr(std::span<const uint8_t> data)
    {
        ByteReader r(data);
        FrameHeader hdr;

        auto w = r.read_u32();
        if (!w)
            return std::unexpected(w.error());
        hdr.width = *w;
        auto h = r.read_u32();
        if (!h)
            return std::unexpected(h.error());
        hdr.height = *h;
        auto bd = r.read_u8();
        if (!bd)
            return std::unexpected(bd.error());
        hdr.bit_depth = *bd;
        auto cp = r.read_u8();
        if (!cp)
            return std::unexpected(cp.error());
        hdr.cicp_primaries = *cp;
        auto ct = r.read_u8();
        if (!ct)
            return std::unexpected(ct.error());
        hdr.cicp_transfer = *ct;
        auto cm = r.read_u8();
        if (!cm)
            return std::unexpected(cm.error());
        hdr.cicp_matrix = *cm;
        auto fl = r.read_u16();
        if (!fl)
            return std::unexpected(fl.error());
        hdr.flags = *fl;
        auto ts = r.read_u8();
        if (!ts)
            return std::unexpected(ts.error());
        hdr.tile_size_log2 = *ts;
        auto mcll = r.read_u16();
        if (!mcll)
            return std::unexpected(mcll.error());
        hdr.max_cll = *mcll;
        auto mfall = r.read_u16();
        if (!mfall)
            return std::unexpected(mfall.error());
        hdr.max_fall = *mfall;

        // FHDR is exactly 19 bytes (SPEC section 2): trailing bytes must
        // not be silently ignored, or a longer payload could smuggle data
        // past the header parser.
        if (r.remaining() != 0)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "FHDR payload must be exactly 19 bytes"});
        }

        if (hdr.width == 0 || hdr.height == 0)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader, "zero dimension"});
        }
        // Bounds for allocations driven by the header: decode() builds the
        // output Image (and per-plane buffers) from width/height before
        // any tile is checked, so an unbounded header turns a few header
        // bytes into a multi-GB allocation. 2^28 pixels still covers any
        // realistic still image (16384x16384); anything larger is
        // rejected here, at the single choke point both get_info() and
        // decode() go through.
        constexpr uint32_t kMaxDimension = 65535;
        constexpr uint64_t kMaxPixels = 268435456ull;  // 2^28
        if (hdr.width > kMaxDimension || hdr.height > kMaxDimension ||
            static_cast<uint64_t>(hdr.width) * hdr.height > kMaxPixels)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "image dimensions exceed supported maximum"});
        }
        if (hdr.bit_depth != 8 && hdr.bit_depth != 10 && hdr.bit_depth != 12)
        {
            return std::unexpected(Error{ErrorCode::InvalidBitDepth,
                                         "bit_depth must be 8, 10, or 12"});
        }
        // The converter only implements these CICP values (colorspace.h):
        // anything else used to silently fall back to BT.709 / sRGB,
        // so a file claiming an unsupported matrix or transfer decoded
        // with the wrong colors. Reject it here instead.
        if (hdr.cicp_matrix != 1 && hdr.cicp_matrix != 5 && hdr.cicp_matrix != 6 && hdr.cicp_matrix != 9)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "unknown CICP matrix coefficient"});
        }
        if (hdr.cicp_transfer != 1 && hdr.cicp_transfer != 13 && hdr.cicp_transfer != 16 && hdr.cicp_transfer != 18)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "unknown CICP transfer characteristic"});
        }
        // The value is only signalled, never used in conversion, but
        // a wrong label on an HDR file is exactly the kind of silent
        // error the validation exists to catch.
        if (!cicp_primaries_is_defined(hdr.cicp_primaries))
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "unknown CICP primaries"});
        }
        // The HDR flag must agree with bit_depth/transfer: set exactly
        // when the frame is HDR (see FrameHeader::hdr_flag_expected).
        // A file that disagrees is mislabelled: the flag is wrong, not
        // the frame.
        if (hdr.is_hdr() != hdr.hdr_flag_expected())
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "HDR flag disagrees with bit_depth/transfer"});
        }
        if (hdr.tile_size_log2 < 6 || hdr.tile_size_log2 > 10)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "tile_size_log2 must be 6..10"});
        }

        return hdr;
    }

    std::vector<uint8_t> serialize_anim(const AnimHeader &anim)
    {
        ByteWriter w;
        w.write_u32(anim.frame_count);
        w.write_u16(anim.loop_count);
        w.write_u32(anim.background_rgba);

        for (const auto &f : anim.frames)
        {
            w.write_u16(f.delay_ms);
            w.write_u8(f.blend_mode);
            w.write_u8(f.disposal);
            w.write_u16(f.rect_x);
            w.write_u16(f.rect_y);
            w.write_u16(f.rect_w);
            w.write_u16(f.rect_h);
            w.write_u32(f.tile_offset);
        }
        return w.finish();
    }

    Result<AnimHeader> parse_anim(std::span<const uint8_t> data)
    {
        ByteReader r(data);
        AnimHeader anim;

        auto fc = r.read_u32();
        if (!fc)
            return std::unexpected(fc.error());
        anim.frame_count = *fc;
        // An animation with no frames is not an animation: the flag
        // claims frames that do not exist, and every consumer would
        // have to handle the degenerate case. #anim #fase9
        if (anim.frame_count == 0)
        {
            return std::unexpected(Error{ErrorCode::AnimationError,
                                         "ANIM frame_count must be at least 1"});
        }
        auto lc = r.read_u16();
        if (!lc)
            return std::unexpected(lc.error());
        anim.loop_count = *lc;
        auto bg = r.read_u32();
        if (!bg)
            return std::unexpected(bg.error());
        anim.background_rgba = *bg;

        // Each frame entry is 16 bytes (2+1+1+2+2+2+2+4). The frame count
        // comes from the file, so prove the payload can hold that many
        // entries before resizing: resizing first lets a 10-byte chunk
        // claiming 0xFFFFFFFF frames abort the decoder with bad_alloc.
        constexpr uint64_t kAnimFrameBytes = 16;
        const uint64_t needed = 10ull + static_cast<uint64_t>(anim.frame_count) * kAnimFrameBytes;
        if (needed > data.size())
        {
            return std::unexpected(Error{ErrorCode::AnimationError,
                                         "ANIM frame_count exceeds payload size"});
        }
        // The payload must hold exactly frame_count entries: trailing
        // bytes after the last frame must not be silently ignored.
        if (needed < data.size())
        {
            return std::unexpected(Error{ErrorCode::AnimationError,
                                         "ANIM payload has trailing bytes"});
        }

        anim.frames.resize(anim.frame_count);
        for (uint32_t i = 0; i < anim.frame_count; i++)
        {
            auto &f = anim.frames[i];
            auto dm = r.read_u16();
            if (!dm)
                return std::unexpected(dm.error());
            f.delay_ms = *dm;
            auto bm = r.read_u8();
            if (!bm)
                return std::unexpected(bm.error());
            f.blend_mode = *bm;
            if (f.blend_mode > 1)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM blend_mode must be 0 or 1"});
            }
            auto ds = r.read_u8();
            if (!ds)
                return std::unexpected(ds.error());
            f.disposal = *ds;
            if (f.disposal > 1)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM disposal must be 0 or 1"});
            }
            auto rx = r.read_u16();
            if (!rx)
                return std::unexpected(rx.error());
            f.rect_x = *rx;
            auto ry = r.read_u16();
            if (!ry)
                return std::unexpected(ry.error());
            f.rect_y = *ry;
            auto rw = r.read_u16();
            if (!rw)
                return std::unexpected(rw.error());
            f.rect_w = *rw;
            auto rh = r.read_u16();
            if (!rh)
                return std::unexpected(rh.error());
            f.rect_h = *rh;
            auto to = r.read_u32();
            if (!to)
                return std::unexpected(to.error());
            f.tile_offset = *to;
        }

        return anim;
    }

    std::vector<uint8_t> serialize_tile_header(const TileHeader &tile)
    {
        ByteWriter w;
        w.write_u16(tile.tile_x);
        w.write_u16(tile.tile_y);
        w.write_u8(tile.layer_flags);
        w.write_u32(tile.compressed_size);
        return w.finish();
    }

    Result<TileHeader> parse_tile_header(std::span<const uint8_t> data)
    {
        ByteReader r(data);
        TileHeader tile;

        auto tx = r.read_u16();
        if (!tx)
            return std::unexpected(tx.error());
        tile.tile_x = *tx;
        auto ty = r.read_u16();
        if (!ty)
            return std::unexpected(ty.error());
        tile.tile_y = *ty;
        auto lf = r.read_u8();
        if (!lf)
            return std::unexpected(lf.error());
        tile.layer_flags = *lf;
        auto cs = r.read_u32();
        if (!cs)
            return std::unexpected(cs.error());
        tile.compressed_size = *cs;

        return tile;
    }

    static void write_chunk(ByteWriter &w, const char type[4], uint8_t flags,
                            std::span<const uint8_t> payload)
    {
        w.write_bytes({reinterpret_cast<const uint8_t *>(type), 4});
        w.write_u8(flags);
        w.write_u32(static_cast<uint32_t>(payload.size()));
        w.write_bytes(payload);
    }

    static Result<Chunk> read_chunk(ByteReader &r)
    {
        Chunk chunk;
        auto type_bytes = r.read_bytes(4);
        if (!type_bytes)
            return std::unexpected(type_bytes.error());
        std::memcpy(chunk.type, type_bytes->data(), 4);

        auto flags = r.read_u8();
        if (!flags)
            return std::unexpected(flags.error());
        chunk.flags = *flags;

        auto size = r.read_u32();
        if (!size)
            return std::unexpected(size.error());

        auto payload = r.read_bytes(*size);
        if (!payload)
            return std::unexpected(payload.error());
        chunk.payload.assign(payload->begin(), payload->end());

        return chunk;
    }

    // Validates an ANIM header against the canvas and the TILE list
    // (SPEC Section 4): frame_count >= 1 and the blend/disposal ranges
    // are already enforced by parse_anim; this checks what parse_anim
    // cannot know — the canvas dimensions and the tile chunks. Runs at
    // the end of parse_container, so every consumer of a .wk file gets
    // the same rejection. #anim #fase9
    [[nodiscard]] Result<void> validate_animation(const WkFile& file)
    {
        const FrameHeader& hdr = file.header;
        const AnimHeader& anim = *file.animation;
        const uint32_t tile_size = hdr.tile_size();
        const uint32_t total_tiles = static_cast<uint32_t>(file.tile_chunks.size());

        // Offsets and rectangles first: they do not depend on the tile
        // headers, and a bad offset must fail before coverage is even
        // attempted.
        for (size_t k = 0; k < anim.frames.size(); ++k)
        {
            const AnimFrame& f = anim.frames[k];
            if (k == 0)
            {
                if (f.tile_offset != 0)
                {
                    return std::unexpected(Error{ErrorCode::AnimationError,
                                                 "first frame tile_offset must be 0"});
                }
            }
            else if (f.tile_offset <= anim.frames[k - 1].tile_offset)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM tile_offset must be strictly increasing"});
            }
            if (f.tile_offset >= total_tiles)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM tile_offset out of range"});
            }
            if (f.rect_w == 0 || f.rect_h == 0)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM frame rect must be non-empty"});
            }
            if (static_cast<uint32_t>(f.rect_x) + f.rect_w > hdr.width ||
                static_cast<uint32_t>(f.rect_y) + f.rect_h > hdr.height)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM frame rect outside canvas"});
            }
            if (f.rect_x % tile_size != 0 || f.rect_y % tile_size != 0)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM frame rect must be tile-aligned"});
            }
            if (static_cast<uint32_t>(f.rect_x) + f.rect_w < hdr.width &&
                f.rect_w % tile_size != 0)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM frame rect width must be tile-aligned"});
            }
            if (static_cast<uint32_t>(f.rect_y) + f.rect_h < hdr.height &&
                f.rect_h % tile_size != 0)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "ANIM frame rect height must be tile-aligned"});
            }
        }

        // Coverage: each frame's TILE chunks must cover its rectangle's
        // tile grid exactly once (the per-frame analog of the still-image
        // grid rule). The count check runs first so a wrong count fails
        // without parsing tile headers.
        for (size_t k = 0; k < anim.frames.size(); ++k)
        {
            const AnimFrame& f = anim.frames[k];
            const uint32_t begin = f.tile_offset;
            const uint32_t end = (k + 1 < anim.frames.size())
                                     ? anim.frames[k + 1].tile_offset
                                     : total_tiles;
            const uint32_t owned = end - begin;

            // Lossless single-tile form (SPEC Section 4.4): one TILE at
            // (0, 0) holding the whole frame. A single (0, 0) tile is
            // always read as the whole frame, so its rectangle must be
            // the full canvas — a sub-rectangle here would be ambiguous.
            if (hdr.is_lossless() && owned == 1)
            {
                auto single = parse_tile_header({file.tile_chunks[begin].payload.data(), 9});
                if (!single) return std::unexpected(single.error());
                if (single->tile_x == 0 && single->tile_y == 0)
                {
                    if (f.rect_x == 0 && f.rect_y == 0 && f.rect_w == hdr.width &&
                        f.rect_h == hdr.height)
                    {
                        continue;
                    }
                    return std::unexpected(Error{ErrorCode::AnimationError,
                                                 "lossless single-tile frame must cover the full canvas"});
                }
            }

            const uint32_t tx0 = f.rect_x / tile_size;
            const uint32_t ty0 = f.rect_y / tile_size;
            const uint32_t tx1 = (static_cast<uint32_t>(f.rect_x) + f.rect_w - 1) / tile_size;
            const uint32_t ty1 = (static_cast<uint32_t>(f.rect_y) + f.rect_h - 1) / tile_size;
            const uint32_t rect_tiles_x = tx1 - tx0 + 1;
            const uint32_t rect_tiles_y = ty1 - ty0 + 1;
            const uint64_t expected = static_cast<uint64_t>(rect_tiles_x) * rect_tiles_y;
            if (owned != expected)
            {
                return std::unexpected(Error{ErrorCode::AnimationError,
                                             "frame TILE count does not match its rect"});
            }
            std::vector<char> seen(static_cast<size_t>(expected), 0);
            for (uint32_t i = begin; i < end; ++i)
            {
                auto tile = parse_tile_header({file.tile_chunks[i].payload.data(), 9});
                if (!tile) return std::unexpected(tile.error());
                if (tile->tile_x < tx0 || tile->tile_x > tx1 || tile->tile_y < ty0 ||
                    tile->tile_y > ty1)
                {
                    return std::unexpected(Error{ErrorCode::AnimationError,
                                                 "frame TILE outside its rect"});
                }
                const size_t index = static_cast<size_t>(tile->tile_y - ty0) * rect_tiles_x +
                                     (tile->tile_x - tx0);
                if (seen[index])
                {
                    return std::unexpected(Error{ErrorCode::AnimationError,
                                                 "duplicate TILE in frame"});
                }
                seen[index] = 1;
            }
        }

        return {};
    }

    Result<WkFile> parse_container(std::span<const uint8_t> data)
    {
        ByteReader r(data);
        WkFile file;

        auto magic = r.read_bytes(5);
        if (!magic)
            return std::unexpected(Error{ErrorCode::InvalidMagic, "too short"});
        if (std::memcmp(magic->data(), WK_MAGIC, 5) != 0)
        {
            return std::unexpected(Error{ErrorCode::InvalidMagic, "not a WK file"});
        }

        auto ver = r.read_u16();
        if (!ver)
            return std::unexpected(ver.error());
        if (*ver != WK_VERSION)
        {
            return std::unexpected(Error{ErrorCode::InvalidVersion,
                                         "unsupported version " + std::to_string(*ver)});
        }

        bool got_fhdr = false;
        bool got_fend = false;
        bool saw_tiles = false;
        bool got_meta = false;
        bool got_iccp = false;
        bool got_prov = false;
        bool got_anim = false;

        while (!r.at_end())
        {
            auto chunk = read_chunk(r);
            if (!chunk)
                return std::unexpected(chunk.error());

            if (!got_fhdr)
            {
                if (!chunk->is_type(CHUNK_FHDR))
                {
                    return std::unexpected(Error{ErrorCode::InvalidHeader, "FHDR must be the first chunk"});
                }
                auto hdr = parse_fhdr(chunk->payload);
                if (!hdr)
                    return std::unexpected(hdr.error());
                file.header = *hdr;
                got_fhdr = true;
                continue;
            }

            if (got_fend)
            {
                return std::unexpected(Error{ErrorCode::InvalidChunkType, "FEND must be the last chunk"});
            }

            if (chunk->is_type(CHUNK_FHDR))
            {
                return std::unexpected(Error{ErrorCode::InvalidHeader, "duplicate FHDR chunk"});
            }
            if (chunk->is_type(CHUNK_WKMETA))
            {
                if (saw_tiles)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "META must precede TILE chunks"});
                }
                // META, ICCP, PROV, and ANIM are singleton chunks (SPEC
                // section 1.3): a second copy must be rejected, not
                // silently overwrite the first.
                if (got_meta)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "duplicate META chunk"});
                }
                // A META chunk that fails to parse must be rejected, not
                // silently dropped: dropping it would discard metadata
                // the file claims to carry.
                //
                // A META chunk with the compressed flag holds a zlib
                // stream, not a WKMETA envelope: decompress first, then
                // parse as usual. A decoder that does not know the flag
                // feeds the raw bytes to the parser, which rejects them
                // (zlib streams start 0x78, never the 0x01 wkmeta
                // version), so old decoders fail cleanly on these files.
                std::span<const uint8_t> meta_payload{chunk->payload};
                std::vector<uint8_t> decompressed;
                if (chunk->flags & CHUNK_FLAG_META_COMPRESSED)
                {
                    auto dec = meta::decompress_meta_payload(chunk->payload);
                    if (!dec)
                    {
                        return std::unexpected(Error{ErrorCode::MetaParseError,
                                                     "META chunk failed to decompress: " +
                                                         dec.error().message});
                    }
                    decompressed = std::move(*dec);
                    meta_payload = std::span<const uint8_t>{decompressed};
                }
                auto meta = meta::MetaBlock::parse(meta_payload);
                if (!meta)
                {
                    return std::unexpected(Error{ErrorCode::MetaParseError,
                                                 "META chunk failed to parse: " + meta.error().message});
                }
                file.metadata = std::move(*meta);
                got_meta = true;
                continue;
            }
            if (chunk->is_type(CHUNK_ICCP))
            {
                if (saw_tiles)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "ICCP must precede TILE chunks"});
                }
                if (got_iccp)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "duplicate ICCP chunk"});
                }
                file.icc_profile = std::move(chunk->payload);
                got_iccp = true;
                continue;
            }
            if (chunk->is_type(CHUNK_PROV))
            {
                if (saw_tiles)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "PROV must precede TILE chunks"});
                }
                if (got_prov)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "duplicate PROV chunk"});
                }
                file.c2pa_manifest = std::move(chunk->payload);
                got_prov = true;
                continue;
            }
            if (chunk->is_type(CHUNK_ANIM))
            {
                if (saw_tiles)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "ANIM must precede TILE chunks"});
                }
                if (got_anim)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkType, "duplicate ANIM chunk"});
                }
                auto anim = parse_anim(chunk->payload);
                if (!anim)
                    return std::unexpected(anim.error());
                file.animation = std::move(*anim);
                got_anim = true;
                continue;
            }
            if (chunk->is_type(CHUNK_TILE))
            {
                if (chunk->payload.size() < 9)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkSize, "TILE chunk too short"});
                }
                auto tile = parse_tile_header({chunk->payload.data(), 9});
                if (!tile)
                    return std::unexpected(tile.error());
                if (chunk->payload.size() - 9 != tile->compressed_size)
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkSize,
                                                 "TILE chunk compressed_size does not match payload"});
                }
                saw_tiles = true;
                file.tile_chunks.push_back(std::move(*chunk));
                continue;
            }
            if (chunk->is_type(CHUNK_FEND))
            {
                if (!chunk->payload.empty())
                {
                    return std::unexpected(Error{ErrorCode::InvalidChunkSize, "FEND payload must be empty"});
                }
                got_fend = true;
                continue;
            }

            if (!(chunk->flags & CHUNK_FLAG_OPTIONAL))
            {
                return std::unexpected(Error{ErrorCode::InvalidChunkType,
                                             std::string("unknown required chunk: ") + std::string(chunk->type, 4)});
            }
        }

        if (!got_fhdr)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader, "missing FHDR chunk"});
        }
        if (!got_fend)
        {
            return std::unexpected(Error{ErrorCode::InvalidChunkType, "missing FEND chunk"});
        }

        // The FHDR flags must agree with the chunks actually present:
        // a flag claiming metadata, animation, or a C2PA manifest that
        // is not in the file (or a chunk the flags do not claim) means
        // the file is malformed, not merely unusual.
        if (file.header.has_wkmeta() != got_meta)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "FHDR HAS_WKMETA flag does not match META chunk presence"});
        }
        if (file.header.is_animated() != got_anim)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "FHDR ANIMATED flag does not match ANIM chunk presence"});
        }
        if (((file.header.flags & FHDR_FLAG_HAS_C2PA) != 0) != got_prov)
        {
            return std::unexpected(Error{ErrorCode::InvalidHeader,
                                         "FHDR HAS_C2PA flag does not match PROV chunk presence"});
        }

        if (file.animation)
        {
            auto anim_valid = validate_animation(file);
            if (!anim_valid) return std::unexpected(anim_valid.error());
        }

        return file;
    }

    Result<std::vector<uint8_t>> write_container(const WkFile &file)
    {
        ByteWriter w;
        w.reserve(1024 * 1024);

        w.write_bytes({WK_MAGIC, 5});
        w.write_u16(WK_VERSION);

        auto fhdr_payload = serialize_fhdr(file.header);
        write_chunk(w, CHUNK_FHDR, 0, fhdr_payload);

        if (file.metadata)
        {
            auto meta_payload = file.metadata->serialize();
            uint8_t meta_flags = CHUNK_FLAG_OPTIONAL | CHUNK_FLAG_META_ONLY;
            // Compress only when strictly smaller: the flag bit costs
            // nothing, so any saving is a net win, and the 15 B baseline
            // (which grows to 23 B compressed) stays raw. ICCP/PROV are
            // never compressed by this slice.
            if (auto compressed = meta::compress_meta_payload(meta_payload))
            {
                meta_payload = std::move(*compressed);
                meta_flags |= CHUNK_FLAG_META_COMPRESSED;
            }
            write_chunk(w, CHUNK_WKMETA, meta_flags, meta_payload);
        }

        if (!file.icc_profile.empty())
        {
            write_chunk(w, CHUNK_ICCP, CHUNK_FLAG_OPTIONAL, file.icc_profile);
        }

        if (!file.c2pa_manifest.empty())
        {
            write_chunk(w, CHUNK_PROV, CHUNK_FLAG_OPTIONAL, file.c2pa_manifest);
        }

        if (file.animation)
        {
            auto anim_payload = serialize_anim(*file.animation);
            write_chunk(w, CHUNK_ANIM, 0, anim_payload);
        }

        for (const auto &tile : file.tile_chunks)
        {
            write_chunk(w, CHUNK_TILE, CHUNK_FLAG_REPEATABLE, tile.payload);
        }

        write_chunk(w, CHUNK_FEND, 0, {});

        return w.finish();
    }

}
