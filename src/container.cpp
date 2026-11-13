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
#include <optional>
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
            auto ds = r.read_u8();
            if (!ds)
                return std::unexpected(ds.error());
            f.disposal = *ds;
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
                auto meta = meta::MetaBlock::parse(chunk->payload);
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
            write_chunk(w, CHUNK_WKMETA, CHUNK_FLAG_OPTIONAL | CHUNK_FLAG_META_ONLY,
                        meta_payload);
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
