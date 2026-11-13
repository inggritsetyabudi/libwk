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
#include "../src/image_io.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "tool_common.h"

namespace {

wk::Result<void> write_text_file(std::string_view path, std::string_view text) {
    std::ofstream file(std::string(path), std::ios::binary);
    if (!file) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to open metadata output"});
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to write metadata output"});
    }
    return {};
}

std::string default_output_path(std::string_view input_path) {
    std::filesystem::path path{std::string(input_path)};
    path.replace_extension(".png");
    return path.string();
}

void print_info(const wk::ImageInfo& info) {
    std::cout
        << "Width:      " << info.width << '\n'
        << "Height:     " << info.height << '\n'
        << "Bit depth:  " << static_cast<unsigned>(info.bit_depth) << '\n'
        << "Alpha:      " << (info.has_alpha ? "yes" : "no") << '\n'
        << "Lossless:   " << (info.is_lossless ? "yes" : "no") << '\n'
        << "Animated:   " << (info.is_animated ? "yes" : "no") << '\n'
        << "HDR:        " << (info.is_hdr ? "yes" : "no") << '\n'
        << "WKMETA:     " << (info.has_wkmeta ? "yes" : "no") << '\n'
        << "Tile size:  " << info.tile_size << '\n'
        << "Frames:     " << info.frame_count << '\n'
        << "CICP:       P=" << static_cast<unsigned>(info.cicp.primaries)
        << " T=" << static_cast<unsigned>(info.cicp.transfer)
        << " M=" << static_cast<unsigned>(info.cicp.matrix)
        << " full_range=" << (info.cicp.full_range ? "yes" : "no") << '\n';
    if (info.max_cll != 0) {
        std::cout << "MaxCLL:     " << info.max_cll << '\n';
    }
    if (info.max_fall != 0) {
        std::cout << "MaxFALL:    " << info.max_fall << '\n';
    }
}

void print_usage() {
    std::cerr
        << "Usage: wkdec [options] <input.wk> [output.{png,ppm}]\n\n"
        << "Options:\n"
        << "  --info               Print image info only\n"
        << "  --export-meta FILE   Export WKMETA as JSON\n"
        << "  --version            Print version\n"
        << "  -h, --help           Show this help\n";
}

}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    std::string metadata_path;
    bool info_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--info") {
            info_only = true;
        } else if (arg == "--export-meta") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --export-meta requires a file path\n";
                return 1;
            }
            metadata_path = argv[++i];
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

    auto input_bytes = wk::tool::read_file_bytes(input_path);
    if (!input_bytes) {
        std::cerr << "Error: " << input_bytes.error().message << '\n';
        return 1;
    }

    if (!metadata_path.empty()) {
        auto parsed = wk::parse_container(*input_bytes);
        if (!parsed) {
            std::cerr << "Error: failed to parse container: " << parsed.error().message << '\n';
            return 1;
        }
        if (!parsed->metadata) {
            std::cerr << "Error: input has no metadata block\n";
            return 1;
        }

        auto write_result = write_text_file(metadata_path, parsed->metadata->to_json());
        if (!write_result) {
            std::cerr << "Error: " << write_result.error().message << '\n';
            return 1;
        }
        std::cerr << "Exported metadata to " << metadata_path << '\n';
    }

    auto info = wk::get_info(*input_bytes);
    if (!info) {
        std::cerr << "Error: failed to read image info: " << info.error().message << '\n';
        return 1;
    }
    if (info_only) {
        print_info(*info);
        return 0;
    }

    if (output_path.empty()) {
        output_path = default_output_path(input_path);
    }

    if (std::filesystem::path(output_path).extension() == ".png" && info->bit_depth != wk::BitDepth::Bits8) {
        std::cerr << "Error: PNG output is only available for 8-bit images; use .ppm for now\n";
        return 1;
    }

    auto image = wk::decode(*input_bytes);
    if (!image) {
        std::cerr << "Error: decode failed: " << image.error().message << '\n';
        return 1;
    }

    auto save_result = wk::io::save_image_file(output_path, *image);
    if (!save_result) {
        std::cerr << "Error: failed to write output image: " << save_result.error().message << '\n';
        return 1;
    }

    std::cerr << "Decoded " << input_path << " -> " << output_path
              << " (" << image->width() << 'x' << image->height() << ")\n";
    return 0;
}
