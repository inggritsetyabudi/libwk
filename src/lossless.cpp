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

#include "lossless.h"
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wk {



void apply_subtract_green(uint8_t* rgba, uint32_t width, uint32_t height) {
    size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; i++) {
        uint8_t g = rgba[i * 4 + 1];
        rgba[i * 4 + 0] = static_cast<uint8_t>((rgba[i * 4 + 0] - g) & 0xFF);
        rgba[i * 4 + 2] = static_cast<uint8_t>((rgba[i * 4 + 2] - g) & 0xFF);
    }
}

void undo_subtract_green(uint8_t* rgba, uint32_t width, uint32_t height) {
    size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; i++) {
        uint8_t g = rgba[i * 4 + 1];
        rgba[i * 4 + 0] = static_cast<uint8_t>((rgba[i * 4 + 0] + g) & 0xFF);
        rgba[i * 4 + 2] = static_cast<uint8_t>((rgba[i * 4 + 2] + g) & 0xFF);
    }
}

bool build_palette(const uint8_t* rgba, uint32_t width, uint32_t height,
                   Palette& palette) {
    std::unordered_map<uint32_t, int> color_map;
    size_t count = static_cast<size_t>(width) * height;

    for (size_t i = 0; i < count; i++) {
        uint32_t color;
        std::memcpy(&color, rgba + i * 4, 4);
        if (color_map.find(color) == color_map.end()) {
            if (color_map.size() >= 256) return false;
            int idx = static_cast<int>(color_map.size());
            color_map[color] = idx;
        }
    }

    palette.count = static_cast<int>(color_map.size());
    for (const auto& [color, idx] : color_map) {
        palette.colors[idx] = color;
    }
    return true;
}

void apply_palette(const uint8_t* rgba, uint32_t width, uint32_t height,
                   const Palette& palette, uint8_t* indices) {
    std::unordered_map<uint32_t, uint8_t> color_to_idx;
    for (int i = 0; i < palette.count; i++) {
        color_to_idx[palette.colors[i]] = static_cast<uint8_t>(i);
    }

    size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; i++) {
        uint32_t color;
        std::memcpy(&color, rgba + i * 4, 4);
        indices[i] = color_to_idx[color];
    }
}

void undo_palette(const uint8_t* indices, uint32_t width, uint32_t height,
                  const Palette& palette, uint8_t* rgba) {
    size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; i++) {
        uint32_t color = palette.colors[indices[i]];
        std::memcpy(rgba + i * 4, &color, 4);
    }
}



std::vector<LzToken> lz_encode_indices(const uint8_t* indices, size_t count) {
    std::vector<LzToken> tokens;
    tokens.reserve(count);

    size_t i = 0;
    while (i < count) {

        size_t best_len = 0;
        size_t best_dist = 0;

        size_t max_dist = std::min(i, static_cast<size_t>(32768));
        for (size_t d = 1; d <= max_dist && d <= 4096; d++) {
            size_t len = 0;
            while (i + len < count && len < 258 &&
                   indices[i + len] == indices[i - d + len]) {
                len++;
            }
            if (len > best_len) {
                best_len = len;
                best_dist = d;
            }
        }

        if (best_len >= 3) {
            tokens.push_back({LzToken::BACKREFERENCE, 0,
                              static_cast<uint16_t>(best_dist),
                              static_cast<uint16_t>(best_len)});
            i += best_len;
        } else {
            tokens.push_back({LzToken::LITERAL, indices[i], 0, 0});
            i++;
        }
    }

    return tokens;
}

Result<void> lz_decode_indices(const std::vector<LzToken>& tokens,
                               uint8_t* indices, size_t count) {
    size_t pos = 0;
    for (const auto& tok : tokens) {
        if (tok.type == LzToken::LITERAL) {
            if (pos >= count) {
                return std::unexpected(Error{ErrorCode::LosslessError,
                    "LZ literal exceeds pixel count"});
            }
            indices[pos++] = tok.literal;
        } else if (tok.type == LzToken::BACKREFERENCE) {
            if (tok.distance == 0 || tok.distance > pos) {
                return std::unexpected(Error{ErrorCode::LosslessError,
                    "LZ backref distance out of range"});
            }
            if (tok.length > count - pos) {
                return std::unexpected(Error{ErrorCode::LosslessError,
                    "LZ backref exceeds pixel count"});
            }
            for (uint16_t j = 0; j < tok.length; j++) {
                indices[pos] = indices[pos - tok.distance];
                pos++;
            }
        } else {
            return std::unexpected(Error{ErrorCode::LosslessError,
                "unknown LZ token type"});
        }
    }
    if (pos != count) {
        return std::unexpected(Error{ErrorCode::LosslessError,
            "LZ stream produced fewer indices than pixels"});
    }
    return {};
}



EntropyImage build_entropy_image(const uint8_t* rgba, uint32_t width, uint32_t height,
                                  int max_context_sets) {
    EntropyImage ei;
    ei.subsample_log2 = 4;
    uint32_t k = 1u << ei.subsample_log2;
    ei.meta_width = (width + k - 1) / k;
    ei.meta_height = (height + k - 1) / k;
    ei.context_map.resize(ei.meta_width * ei.meta_height);



    max_context_sets = std::min(max_context_sets, 12);

    std::vector<float> entropies(ei.meta_width * ei.meta_height);
    float max_entropy = 0;

    for (uint32_t my = 0; my < ei.meta_height; my++) {
        for (uint32_t mx = 0; mx < ei.meta_width; mx++) {

            uint32_t hist[256] = {};
            uint32_t total = 0;

            uint32_t y0 = my * k, y1 = std::min(y0 + k, height);
            uint32_t x0 = mx * k, x1 = std::min(x0 + k, width);

            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {

                    hist[rgba[(y * width + x) * 4 + 1]]++;
                    total++;
                }
            }


            float entropy = 0;
            for (int i = 0; i < 256; i++) {
                if (hist[i] > 0) {
                    float p = static_cast<float>(hist[i]) / total;
                    entropy -= p * std::log2(p);
                }
            }
            entropies[my * ei.meta_width + mx] = entropy;
            max_entropy = std::max(max_entropy, entropy);
        }
    }


    ei.num_context_sets = max_context_sets;
    for (size_t i = 0; i < entropies.size(); i++) {
        float normalized = (max_entropy > 0) ? entropies[i] / max_entropy : 0;
        int ctx = static_cast<int>(normalized * (max_context_sets - 1) + 0.5f);
        ei.context_map[i] = static_cast<uint8_t>(
            std::clamp(ctx, 0, max_context_sets - 1));
    }

    return ei;
}



static inline uint8_t predict_pixel(LosslessPred mode, uint8_t l, uint8_t t,
                                     uint8_t tr, uint8_t tl) {
    switch (mode) {
        case LosslessPred::NONE:     return 0;
        case LosslessPred::L:        return l;
        case LosslessPred::T:        return t;
        case LosslessPred::TR:       return tr;
        case LosslessPred::TL:       return tl;
        case LosslessPred::AVG_LT:   return static_cast<uint8_t>((l + t + 1) / 2);
        case LosslessPred::AVG_LTR:  return static_cast<uint8_t>((l + t + tr + 1) / 3);
        case LosslessPred::AVG_LTL:  return static_cast<uint8_t>((l + tl + 1) / 2);
        case LosslessPred::AVG_TTR:  return static_cast<uint8_t>((t + tr + 1) / 2);
        case LosslessPred::AVG_TTL:  return static_cast<uint8_t>((t + tl + 1) / 2);
        case LosslessPred::AVG_LTRTL:return static_cast<uint8_t>((l + t + tr + tl + 2) / 4);
        case LosslessPred::SELECT: {

            int p = l + t - tl;
            int pa = std::abs(p - l);
            int pb = std::abs(p - t);
            int pc = std::abs(p - tl);
            if (pa <= pb && pa <= pc) return l;
            if (pb <= pc) return t;
            return tl;
        }
        case LosslessPred::CLAMP_ADD: {
            int v = static_cast<int>(l) + t - tl;
            return static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
        default: return 0;
    }
}



Result<std::vector<uint8_t>> lossless_encode(
    const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t bit_depth) {

    if (bit_depth != 8) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature,
            "lossless currently only supports 8-bit"});
    }

    size_t pixel_count = static_cast<size_t>(width) * height;


    std::vector<uint8_t> transformed(pixel_count * 4);
    std::memcpy(transformed.data(), rgba, pixel_count * 4);
    apply_subtract_green(transformed.data(), width, height);


    Palette palette;
    bool use_palette = build_palette(rgba, width, height, palette);



    LosslessPred pred_mode = LosslessPred::SELECT;


    ByteWriter header;
    header.write_u8(static_cast<uint8_t>(use_palette ? 1 : 0));
    header.write_u8(static_cast<uint8_t>(pred_mode));

    if (use_palette) {
        header.write_u16(static_cast<uint16_t>(palette.count));
        for (int i = 0; i < palette.count; i++) {
            header.write_u32(palette.colors[i]);
        }


        std::vector<uint8_t> indices(pixel_count);
        apply_palette(rgba, width, height, palette, indices.data());

        auto tokens = lz_encode_indices(indices.data(), pixel_count);


        header.write_u32(static_cast<uint32_t>(tokens.size()));
        for (const auto& tok : tokens) {
            header.write_u8(static_cast<uint8_t>(tok.type));
            if (tok.type == LzToken::LITERAL) {
                header.write_u8(tok.literal);
            } else {
                header.write_u16(tok.distance);
                header.write_u16(tok.length);
            }
        }
    } else {

        for (int ch = 0; ch < 4; ch++) {

            std::vector<uint8_t> residuals(pixel_count);

            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    size_t idx = y * width + x;
                    uint8_t val = transformed[idx * 4 + ch];

                    uint8_t l  = (x > 0) ? transformed[(idx - 1) * 4 + ch] : 0;
                    uint8_t t  = (y > 0) ? transformed[(idx - width) * 4 + ch] : 0;
                    uint8_t tr = (y > 0 && x < width - 1) ?
                                  transformed[(idx - width + 1) * 4 + ch] : t;
                    uint8_t tl = (y > 0 && x > 0) ?
                                  transformed[(idx - width - 1) * 4 + ch] : 0;

                    uint8_t predicted = predict_pixel(pred_mode, l, t, tr, tl);
                    residuals[idx] = static_cast<uint8_t>((val - predicted) & 0xFF);
                }
            }


            uint32_t counts[256] = {};
            for (size_t i = 0; i < pixel_count; i++) {
                counts[residuals[i]]++;
            }


            RansTable<RANS_PRECISION_BITS> table;
            table.build_from_counts(counts, 256);


            for (int i = 0; i < 256; i++) {
                header.write_u16(table.symbol(i).freq);
            }


            RansEncoder<RANS_PRECISION_BITS> enc;
            enc.init();

            for (size_t i = pixel_count; i > 0; i--) {
                enc.encode(table, residuals[i - 1]);
            }
            auto encoded = enc.finish();

            header.write_u32(static_cast<uint32_t>(encoded.size()));
            header.write_bytes(encoded);
        }
    }

    return header.finish();
}



Result<std::vector<uint8_t>> lossless_decode(
    std::span<const uint8_t> data, uint32_t width, uint32_t height,
    uint8_t bit_depth) {

    if (bit_depth != 8) {
        return std::unexpected(Error{ErrorCode::UnsupportedFeature,
            "lossless currently only supports 8-bit"});
    }

    ByteReader reader(data);
    size_t pixel_count = static_cast<size_t>(width) * height;

    auto use_palette_r = reader.read_u8();
    if (!use_palette_r) return std::unexpected(use_palette_r.error());
    bool use_palette = *use_palette_r != 0;

    auto pred_mode_r = reader.read_u8();
    if (!pred_mode_r) return std::unexpected(pred_mode_r.error());
    LosslessPred pred_mode = static_cast<LosslessPred>(*pred_mode_r);

    std::vector<uint8_t> output(pixel_count * 4);

    if (use_palette) {
        auto count_r = reader.read_u16();
        if (!count_r) return std::unexpected(count_r.error());

        // The palette array holds 256 entries; the count comes straight
        // from the file, so reject anything it cannot hold before writing
        // a single color. Zero is invalid too: a non-empty image always
        // has at least one palette entry.
        if (*count_r == 0 || *count_r > 256) {
            return std::unexpected(Error{ErrorCode::LosslessError,
                "palette count out of range"});
        }

        Palette palette;
        palette.count = *count_r;
        for (int i = 0; i < palette.count; i++) {
            auto c = reader.read_u32();
            if (!c) return std::unexpected(c.error());
            palette.colors[i] = *c;
        }

        auto token_count_r = reader.read_u32();
        if (!token_count_r) return std::unexpected(token_count_r.error());

        // Each token occupies at least its 1-byte type in the remaining
        // payload, so a larger claim cannot describe this tile. Check
        // before allocating the token vector: the count is a u32 from
        // the file and allocating it blindly is a trivial OOM.
        if (*token_count_r > reader.remaining()) {
            return std::unexpected(Error{ErrorCode::LosslessError,
                "LZ token count exceeds remaining payload"});
        }

        std::vector<LzToken> tokens(*token_count_r);
        for (uint32_t i = 0; i < *token_count_r; i++) {
            auto type_r = reader.read_u8();
            if (!type_r) return std::unexpected(type_r.error());
            tokens[i].type = static_cast<LzToken::Type>(*type_r);

            if (tokens[i].type == LzToken::LITERAL) {
                auto lit = reader.read_u8();
                if (!lit) return std::unexpected(lit.error());
                tokens[i].literal = *lit;
            } else {
                auto dist = reader.read_u16();
                if (!dist) return std::unexpected(dist.error());
                tokens[i].distance = *dist;
                auto len = reader.read_u16();
                if (!len) return std::unexpected(len.error());
                tokens[i].length = *len;
            }
        }

        std::vector<uint8_t> indices(pixel_count);
        auto lz = lz_decode_indices(tokens, indices.data(), pixel_count);
        if (!lz) return std::unexpected(lz.error());
        // Indices are uint8, so they are always in range of the 256-entry
        // array, but only the first palette.count entries were read from
        // the file; the rest are uninitialized. Reject out-of-palette
        // indices instead of decoding with garbage colors.
        for (size_t i = 0; i < pixel_count; ++i) {
            if (indices[i] >= palette.count) {
                return std::unexpected(Error{ErrorCode::LosslessError,
                    "palette index out of range"});
            }
        }
        undo_palette(indices.data(), width, height, palette, output.data());
    } else {

        std::vector<uint8_t> transformed(pixel_count * 4);

        for (int ch = 0; ch < 4; ch++) {

            RansTable<RANS_PRECISION_BITS> table;
            uint32_t counts[256];
            for (int i = 0; i < 256; i++) {
                auto f = reader.read_u16();
                if (!f) return std::unexpected(f.error());
                counts[i] = *f;
            }
            table.build_from_counts(counts, 256);


            auto enc_size = reader.read_u32();
            if (!enc_size) return std::unexpected(enc_size.error());
            auto enc_data = reader.read_bytes(*enc_size);
            if (!enc_data) return std::unexpected(enc_data.error());


            RansDecoder<RANS_PRECISION_BITS> dec;
            dec.init(enc_data->data(), enc_data->size());
            if (!dec.ok()) {
                return std::unexpected(Error{ErrorCode::RansError, "invalid rANS stream header"});
            }

            std::vector<uint8_t> residuals(pixel_count);
            for (size_t i = 0; i < pixel_count; i++) {
                residuals[i] = static_cast<uint8_t>(dec.decode(table));
                if (!dec.ok()) {
                    return std::unexpected(Error{ErrorCode::RansError, "corrupt rANS stream"});
                }
            }


            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    size_t idx = y * width + x;

                    uint8_t l  = (x > 0) ? transformed[(idx - 1) * 4 + ch] : 0;
                    uint8_t t  = (y > 0) ? transformed[(idx - width) * 4 + ch] : 0;
                    uint8_t tr = (y > 0 && x < width - 1) ?
                                  transformed[(idx - width + 1) * 4 + ch] : t;
                    uint8_t tl = (y > 0 && x > 0) ?
                                  transformed[(idx - width - 1) * 4 + ch] : 0;

                    uint8_t predicted = predict_pixel(pred_mode, l, t, tr, tl);
                    transformed[idx * 4 + ch] = static_cast<uint8_t>(
                        (residuals[idx] + predicted) & 0xFF);
                }
            }
        }


        undo_subtract_green(transformed.data(), width, height);
        output = std::move(transformed);
    }

    return output;
}

}
