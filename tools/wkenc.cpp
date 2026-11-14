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

#include <wk/wk.hpp>
#include <wk/wkmeta.hpp>
#include "../src/common.h"
#include "../src/container.h"
#include "../src/exif_import.h"
#include "../src/image_io.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "tool_common.h"

namespace {

std::string default_output_path(std::string_view input_path) {
    std::filesystem::path path{std::string(input_path)};
    path.replace_extension(".wk");
    return path.string();
}

bool is_power_of_two(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

wk::Result<uint8_t> parse_tile_size(std::string_view text) {
    uint32_t value = 0;
    try {
        value = static_cast<uint32_t>(std::stoul(std::string(text)));
    } catch (...) {
        return std::unexpected(wk::Error{wk::ErrorCode::InvalidParameter, "tile size must be an integer"});
    }

    if (value < 64 || value > 1024 || !is_power_of_two(value)) {
        return std::unexpected(wk::Error{wk::ErrorCode::InvalidParameter,
            "tile size must be a power of two between 64 and 1024"});
    }

    uint8_t log2 = 0;
    while ((1u << log2) < value) {
        ++log2;
    }
    return log2;
}

void merge_metadata(wk::meta::MetaBlock& destination, const wk::meta::MetaBlock& source) {
    for (const auto& entry : source.entries) {
        if (entry.is_opaque) {
            destination.remove(entry.ns, entry.tag);
            destination.entries.push_back(entry);
            continue;
        }

        auto set_result = destination.set(entry.ns, entry.tag, entry.value);
        if (!set_result) {
            destination.remove(entry.ns, entry.tag);
            destination.entries.push_back(entry);
        }
    }
}

wk::Result<std::vector<uint8_t>> apply_exif_metadata(std::span<const uint8_t> encoded,
                                                     std::string_view exif_source_path) {
    auto donor_bytes = wk::tool::read_file_bytes(exif_source_path);
    if (!donor_bytes) {
        return std::unexpected(donor_bytes.error());
    }

    auto exif_blob = wk::meta::extract_exif_blob_from_bytes(*donor_bytes);
    if (!exif_blob) {
        return std::unexpected(wk::Error{wk::ErrorCode::MetaParseError, exif_blob.error().message});
    }

    auto imported = wk::meta::MetaBlock::from_exif(*exif_blob);
    if (!imported) {
        return std::unexpected(wk::Error{wk::ErrorCode::MetaParseError, imported.error().message});
    }

    auto parsed = wk::parse_container(encoded);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    if (!parsed->metadata) {
        parsed->metadata = wk::meta::MetaBlock{};
    }
    merge_metadata(*parsed->metadata, *imported);

    if (parsed->metadata && !parsed->metadata->entries.empty()) {
        parsed->header.flags |= wk::FHDR_FLAG_HAS_WKMETA;
    }

    auto rewritten = wk::write_container(*parsed);
    if (!rewritten) {
        return std::unexpected(rewritten.error());
    }
    return *rewritten;
}

void print_usage() {
    std::cerr
        << "Usage: wkenc [options] <input.{jpg,jpeg,png,ppm}> [output.wk]\n\n"
        << "Options:\n"
        << "  --quality N             Lossy quality 0..100 (default: 75)\n"
        << "  --lossless              Encode losslessly\n"
        << "  --tile-size N           Explicit tile size in pixels (64..1024, power of two)\n"
        << "  --threads N             Thread count (0 = auto)\n"
        << "  --yuv444                Use 4:4:4 chroma (default)\n"
        << "  --yuv420                Use 4:2:0 chroma\n"
        << "  --tile-stats            Print a per-tile byte breakdown (tables vs coded data)\n"
        << "  --coeff-stats           Print per-coefficient-position stream sizes\n"
        << "  --import-exif FILE      Import EXIF from JPEG/TIFF donor file\n"
        << "  --version               Print version\n"
        << "  -h, --help              Show this help\n\n"
        << "Defaults:\n"
        << "  Lossy encode uses 1024px tiles unless --tile-size is set\n"
        << "  Lossless encode keeps the legacy 512px tile header unless overridden\n";
}

}

void print_tile_stats(const wk::EncodeStats& stats) {
    // Per-tile byte breakdown, with the coefficient-payload split that
    // decides Phase 1 priorities (tables vs coded data).
    for (size_t i = 0; i < stats.tiles.size(); ++i) {
        const auto& t = stats.tiles[i];
        const auto pct = [&](size_t n) {
            return t.total_bytes == 0 ? 0.0 : 100.0 * static_cast<double>(n) / t.total_bytes;
        };
        std::cerr << "tile " << i << ": " << t.total_bytes << " B\n"
                  << "  quant tables : " << t.quant_table_bytes << " B (" << pct(t.quant_table_bytes) << "%)\n"
                  << "  layout       : " << t.layout_bytes << " B (" << pct(t.layout_bytes) << "%)\n"
                  << "  V7 scales    : " << t.v7_scale_bytes << " B (" << pct(t.v7_scale_bytes) << "%)\n"
                  << "  modes        : " << t.mode_bytes << " B (" << pct(t.mode_bytes) << "%)\n"
                  << "  spans        : " << t.span_bytes << " B (" << pct(t.span_bytes) << "%)\n"
                  << "  max spans    : " << t.max_span_bytes << " B (" << pct(t.max_span_bytes) << "%)\n"
                  << "  Y coeff tbl  : " << t.y_coeff_table_bytes << " B (" << pct(t.y_coeff_table_bytes) << "%)\n"
                  << "  Y coeff data : " << t.y_coeff_stream_bytes << " B (" << pct(t.y_coeff_stream_bytes) << "%)\n"
                  << "  Y coeff flag : " << t.y_coeff_flag_bytes << " B (" << pct(t.y_coeff_flag_bytes) << "%)\n"
                  << "  C coeff tbl  : " << t.chroma_coeff_table_bytes << " B (" << pct(t.chroma_coeff_table_bytes) << "%)\n"
                  << "  C coeff data : " << t.chroma_coeff_stream_bytes << " B (" << pct(t.chroma_coeff_stream_bytes) << "%)\n"
                  << "  C coeff flag : " << t.chroma_coeff_flag_bytes << " B (" << pct(t.chroma_coeff_flag_bytes) << "%)\n";
        if (t.alpha_coeff_table_bytes + t.alpha_coeff_stream_bytes + t.alpha_coeff_flag_bytes > 0) {
            std::cerr << "  A coeff tbl  : " << t.alpha_coeff_table_bytes << " B (" << pct(t.alpha_coeff_table_bytes) << "%)\n"
                      << "  A coeff data : " << t.alpha_coeff_stream_bytes << " B (" << pct(t.alpha_coeff_stream_bytes) << "%)\n"
                      << "  A coeff flag : " << t.alpha_coeff_flag_bytes << " B (" << pct(t.alpha_coeff_flag_bytes) << "%)\n";
        }
    }
}

// Per-coefficient-position stream sizes, aggregated across tiles, for
// the coeff-squeezing work: shows which zigzag positions eat the bits
// (0 = DC, 63 = highest frequency).
void print_coeff_stats(const wk::EncodeStats& stats) {
    std::array<size_t, 64> y{};
    std::array<size_t, 64> c{};
    size_t y_total = 0;
    size_t c_total = 0;
    for (const auto& t : stats.tiles) {
        for (size_t i = 0; i < 64; ++i) {
            y[i] += t.y_position_stream_bytes[i];
            c[i] += t.chroma_position_stream_bytes[i];
        }
        y_total += t.y_coeff_stream_bytes;
        c_total += t.chroma_coeff_stream_bytes;
    }
    const size_t total = y_total + c_total;
    size_t sign_total = 0;
    for (const auto& t : stats.tiles) sign_total += t.y_sign_bytes + t.chroma_sign_bytes;
    std::cerr << "coeff position stream bytes (Y + chroma, all tiles):\n";
    std::cerr << " sign bits total: " << sign_total << " B ("
              << (total == 0 ? 0.0 : 100.0 * static_cast<double>(sign_total) / total)
              << "% of coeff stream)\n";
    std::cerr << " pos      Y B    C B  total B   share\n";
    for (size_t i = 0; i < 64; ++i) {
        const size_t s = y[i] + c[i];
        if (s == 0) continue;
        std::cerr << " " << i << "  " << y[i] << "  " << c[i] << "  " << s << "  "
                  << (total == 0 ? 0.0 : 100.0 * static_cast<double>(s) / total) << "%\n";
    }
    std::cerr << " Y total " << y_total << " B, chroma total " << c_total << " B\n";
}


int main(int argc, char* argv[]) {
    wk::EncoderConfig config;
    std::string input_path;
    std::string output_path;
    std::string exif_source_path;
    bool tile_stats = false;
    bool coeff_stats = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--quality") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --quality requires a value\n";
                return 1;
            }
            try {
                config.quality = std::stof(argv[++i]);
            } catch (...) {
                std::cerr << "Error: invalid quality value\n";
                return 1;
            }
            if (config.quality < 0.0f || config.quality > 100.0f) {
                std::cerr << "Error: quality must be in the range 0..100\n";
                return 1;
            }
        } else if (arg == "--lossless") {
            config.lossless = true;
        } else if (arg == "--tile-size") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --tile-size requires a value\n";
                return 1;
            }
            auto tile_size = parse_tile_size(argv[++i]);
            if (!tile_size) {
                std::cerr << "Error: " << tile_size.error().message << '\n';
                return 1;
            }
            config.tile_size_log2 = *tile_size;
        } else if (arg == "--threads") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --threads requires a value\n";
                return 1;
            }
            try {
                config.threads = static_cast<uint32_t>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "Error: invalid thread count\n";
                return 1;
            }
        } else if (arg == "--yuv444") {
            config.subsampling = wk::Subsampling::YUV444;
        } else if (arg == "--yuv420") {
            config.subsampling = wk::Subsampling::YUV420;
        } else if (arg == "--tile-stats") {
            tile_stats = true;
        } else if (arg == "--coeff-stats") {
            coeff_stats = true;
            tile_stats = true;  // coeff stats ride on the same EncodeStats
        } else if (arg == "--import-exif") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --import-exif requires a donor file\n";
                return 1;
            }
            exif_source_path = argv[++i];
        } else if (arg == "--version") {
            std::cout << wk::version() << '\n';
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option " << arg << '\n';
            print_usage();
            return 1;
        } else if (input_path.empty()) {
            input_path = arg;
        } else if (output_path.empty()) {
            output_path = arg;
        } else {
            std::cerr << "Error: unexpected extra argument " << arg << '\n';
            return 1;
        }
    }

    if (input_path.empty()) {
        print_usage();
        return 1;
    }
    if (output_path.empty()) {
        output_path = default_output_path(input_path);
    }

    auto image = wk::io::load_image_file(input_path);
    if (!image) {
        std::cerr << "Error: failed to load input image: " << image.error().message << '\n';
        return 1;
    }

    std::vector<uint8_t> output_bytes;
    if (tile_stats) {
        wk::EncodeStats stats;
        auto encoded = wk::encode_with_stats(*image, config, stats);
        if (!encoded) {
            std::cerr << "Error: encode failed: " << encoded.error().message << '\n';
            return 1;
        }
        output_bytes = std::move(*encoded);
        print_tile_stats(stats);
        if (coeff_stats) print_coeff_stats(stats);
    } else {
        auto encoded = wk::encode(*image, config);
        if (!encoded) {
            std::cerr << "Error: encode failed: " << encoded.error().message << '\n';
            return 1;
        }
        output_bytes = std::move(*encoded);
    }

    if (!exif_source_path.empty()) {
        auto with_exif = apply_exif_metadata(output_bytes, exif_source_path);
        if (!with_exif) {
            std::cerr << "Error: EXIF import failed: " << with_exif.error().message << '\n';
            return 1;
        }
        output_bytes = std::move(*with_exif);
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        std::cerr << "Error: failed to open output file\n";
        return 1;
    }
    output.write(reinterpret_cast<const char*>(output_bytes.data()), static_cast<std::streamsize>(output_bytes.size()));
    if (!output) {
        std::cerr << "Error: failed to write output file\n";
        return 1;
    }

    const double raw_bytes = static_cast<double>(image->pixels().size());
    const double ratio = output_bytes.empty() ? 0.0 : raw_bytes / static_cast<double>(output_bytes.size());
    std::cerr << "Encoded " << input_path << " -> " << output_path
              << " (" << image->width() << 'x' << image->height()
              << ", " << (config.lossless ? "lossless" : "lossy")
              << ", " << output_bytes.size() << " bytes, ratio " << ratio << "x)\n";
    return 0;
}
