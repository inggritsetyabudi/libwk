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
#include "../src/core/common.h"
#include "../src/core/container.h"
#include "../src/core/image_io.h"
#include "../src/core/metrics.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "tool_common.h"
#include "tool_log.h"

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
        << "  --frame N            Decode frame N of an animated file (default: frame 0)\n"
        << "  --export-meta FILE   Export WKMETA as JSON\n"
        << "  --metrics FILE       Decode, then report PSNR/SSIM vs FILE (reference image)\n"
        << "  -v, --verbose        Show debug logs (per-step detail)\n"
        << "  -q, --quiet          Only warnings and errors\n"
        << "  --color              Force colored log output\n"
        << "  --no-color           Disable colored log output\n"
        << "  --version            Print version\n"
        << "  -h, --help           Show this help\n";
}

}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    std::string metadata_path;
    std::string metrics_path;
    std::optional<uint32_t> frame_index;
    bool info_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--info") {
            info_only = true;
        } else if (arg == "--frame") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --frame requires a frame index\n";
                return 1;
            }
            const std::string value = argv[++i];
            uint32_t parsed = 0;
            const auto res = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (res.ec != std::errc() || res.ptr != value.data() + value.size()) {
                std::cerr << "Error: --frame requires a non-negative integer, got '" << value << "'\n";
                return 1;
            }
            frame_index = parsed;
        } else if (arg == "--export-meta") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --export-meta requires a file path\n";
                return 1;
            }
            metadata_path = argv[++i];
        } else if (arg == "--metrics") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --metrics requires a reference file\n";
                return 1;
            }
            metrics_path = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            wk::tool::set_log_level(wk::tool::LogLevel::Debug);
        } else if (arg == "-q" || arg == "--quiet") {
            wk::tool::set_log_level(wk::tool::LogLevel::Warn);
        } else if (arg == "--color") {
            wk::tool::set_log_color(true);
        } else if (arg == "--no-color") {
            wk::tool::set_log_color(false);
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
        wk::tool::log_error(input_bytes.error().message + ": " + input_path);
        return 1;
    }
    wk::tool::log_info("loaded " + input_path + " (" + std::to_string(input_bytes->size()) + " B)");

    if (!metadata_path.empty()) {
        auto parsed = wk::parse_container(*input_bytes);
        if (!parsed) {
            wk::tool::log_error("failed to parse container: " + parsed.error().message);
            return 1;
        }
        if (!parsed->metadata) {
            wk::tool::log_error("input has no metadata block");
            return 1;
        }

        auto write_result = write_text_file(metadata_path, parsed->metadata->to_json());
        if (!write_result) {
            wk::tool::log_error(write_result.error().message);
            return 1;
        }
        wk::tool::log_info("exported metadata to " + metadata_path);
    }

    auto info = wk::get_info(*input_bytes);
    if (!info) {
        wk::tool::log_error("failed to read image info: " + info.error().message);
        return 1;
    }
    if (info_only) {
        print_info(*info);
        return 0;
    }
    wk::tool::log_debug(std::string("container: ") + std::to_string(info->width) + "x" +
                        std::to_string(info->height) + ", " +
                        (info->is_lossless ? "lossless" : "lossy") + ", tile " +
                        std::to_string(info->tile_size) + "px");

    if (output_path.empty()) {
        output_path = default_output_path(input_path);
    }

    if (std::filesystem::path(output_path).extension() == ".png" && info->bit_depth != wk::BitDepth::Bits8) {
        wk::tool::log_error("PNG output is only available for 8-bit images; use .ppm for now");
        return 1;
    }

    // Without --frame an animated file decodes as frame 0 (the composited
    // first frame); the log line says so and points at --frame. A still
    // image ignores --frame 0 and rejects any other index, via
    // decode_frame itself. #anim #fase9
    if (!frame_index.has_value() && info->is_animated) {
        wk::tool::log_info("animated file with " + std::to_string(info->frame_count) +
                           " frames; decoded frame 0 (use --frame N for another frame)");
    }
    auto image = frame_index.has_value() ? wk::decode_frame(*input_bytes, *frame_index)
                                         : wk::decode(*input_bytes);
    if (!image) {
        wk::tool::log_error("decode failed: " + image.error().message);
        return 1;
    }

    auto save_result = wk::io::save_image_file(output_path, *image);
    if (!save_result) {
        wk::tool::log_error("failed to write output image: " + save_result.error().message);
        return 1;
    }

    wk::tool::log_ok("decoded " + input_path + " -> " + output_path + " (" +
                     std::to_string(image->width()) + "x" + std::to_string(image->height()) + ")" +
                     (frame_index.has_value() ? " frame " + std::to_string(*frame_index) : ""));

    if (!metrics_path.empty()) {
        // Decode + metrics in one run: a bad decode is visible at once.
        auto reference = wk::io::load_image_file(metrics_path);
        if (!reference) {
            wk::tool::log_warn("metrics skipped: could not load reference: " + reference.error().message);
            return 0;
        }
        auto m = wk::metrics::compare_images(*reference, *image);
        if (!m) {
            wk::tool::log_warn("metrics skipped: compare failed: " + m.error().message);
            return 0;
        }
        wk::tool::log_info("metrics: PSNR " + wk::tool::color_psnr(m->psnr, wk::tool::log_color_enabled()) +
                           " dB, SSIM " + wk::tool::color_ssim(m->ssim, wk::tool::log_color_enabled()));
    }
    return 0;
}
