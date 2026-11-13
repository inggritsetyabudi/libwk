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
#include "../src/image_io.h"
#include "../src/metrics.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "tool_common.h"

namespace {

std::string lowercase_extension(std::string_view path) {
    std::filesystem::path fs_path{std::string(path)};
    std::string ext = fs_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

wk::Result<wk::Image> load_visual_input(std::string_view path) {
    if (lowercase_extension(path) == ".wk") {
        auto encoded = wk::tool::read_file_bytes(path);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto decoded = wk::decode(*encoded);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return *decoded;
    }
    return wk::io::load_image_file(path);
}

uintmax_t file_size_or_zero(std::string_view path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(std::filesystem::path(std::string(path)), ec);
    return ec ? 0u : size;
}

std::string channel_label(uint8_t index) {
    switch (index) {
    case 0:
        return "R";
    case 1:
        return "G";
    case 2:
        return "B";
    case 3:
        return "A";
    default:
        return "?";
    }
}

std::string plane_label(uint8_t index) {
    switch (index) {
    case 0:
        return "Y";
    case 1:
        return "Cb";
    case 2:
        return "Cr";
    default:
        return "?";
    }
}

std::string json_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const unsigned char ch : input) {
        switch (ch) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output.push_back(static_cast<char>(ch));
            break;
        }
    }
    return output;
}

void print_image_statistics_text(std::string_view label, const wk::metrics::ImageStatistics& stats) {
    std::cout
        << label
        << " LUMA=" << stats.mean_luma
        << " STD=" << stats.luma_stddev
        << " CHROMA=" << stats.mean_chroma
        << " DARK=" << stats.dark_fraction
        << " BRIGHT=" << stats.bright_fraction
        << '\n';
}

void print_image_statistics_json(std::string_view key, const wk::metrics::ImageStatistics& stats, bool trailing_comma) {
    std::cout
        << "  \"" << key << "\": {\n"
        << "    \"mean_luma\": " << stats.mean_luma << ",\n"
        << "    \"luma_stddev\": " << stats.luma_stddev << ",\n"
        << "    \"mean_chroma\": " << stats.mean_chroma << ",\n"
        << "    \"dark_fraction\": " << stats.dark_fraction << ",\n"
        << "    \"bright_fraction\": " << stats.bright_fraction << "\n"
        << "  }";
    if (trailing_comma) {
        std::cout << ',';
    }
    std::cout << '\n';
}

void print_usage() {
    std::cerr
        << "Usage: wkmetric [options] <reference.{wk,jpg,jpeg,png,ppm}> <candidate.{wk,jpg,jpeg,png,ppm}>\n\n"
        << "Options:\n"
        << "  --json      Print metrics as JSON\n"
        << "  --rgb-only  Ignore alpha even if both inputs have it\n";
}

void print_text_report(std::string_view reference_path,
                       std::string_view candidate_path,
                       const wk::metrics::ImageMetrics& metrics,
                       uintmax_t reference_size,
                       uintmax_t candidate_size,
                       uint8_t bit_depth) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout
        << "Reference:      " << reference_path << '\n'
        << "Candidate:      " << candidate_path << '\n'
        << "Dimensions:     " << metrics.width << 'x' << metrics.height << '\n'
        << "Bit depth:      " << static_cast<unsigned>(bit_depth) << '\n'
        << "Channels:       " << (metrics.compared_alpha ? "RGBA" : "RGB") << '\n'
        << "Reference bytes:" << ' ' << reference_size << '\n'
        << "Candidate bytes:" << ' ' << candidate_size << '\n';
    if (reference_size > 0) {
        std::cout << "Size ratio:     " << static_cast<double>(candidate_size) / static_cast<double>(reference_size) << "x\n";
    }
    std::cout
        << "MAE:            " << metrics.mae << '\n'
        << "MSE:            " << metrics.mse << '\n'
        << "PSNR:           " << metrics.psnr << " dB\n"
        << "SSIM:           " << metrics.ssim << '\n';

    for (uint8_t channel_index = 0; channel_index < metrics.compared_channels; ++channel_index) {
        const auto& channel = metrics.channels[channel_index];
        std::cout
            << channel_label(channel_index) << ":"
            << " MAE=" << channel.mae
            << " MSE=" << channel.mse
            << " PSNR=" << channel.psnr
            << " SSIM=" << channel.ssim
            << '\n';
    }

    std::cout << "YCbCr:\n";
    for (size_t plane_index = 0; plane_index < metrics.ycbcr.size(); ++plane_index) {
        const auto& plane = metrics.ycbcr[plane_index];
        std::cout
            << plane_label(static_cast<uint8_t>(plane_index)) << ":"
            << " MAE=" << plane.mae
            << " MSE=" << plane.mse
            << " PSNR=" << plane.psnr
            << " SSIM=" << plane.ssim
            << '\n';
    }

    std::cout
        << "Chroma MAE:     " << metrics.artifacts.chroma_mae << '\n'
        << "Chroma PSNR:    " << metrics.artifacts.chroma_psnr << " dB\n"
        << "Weighted Luma:  " << metrics.artifacts.weighted_luma_mae << '\n'
        << "Weighted Chroma:" << ' ' << metrics.artifacts.weighted_chroma_mae << '\n'
        << "Max Abs Error:  " << metrics.artifacts.max_abs_error << '\n';

    print_image_statistics_text("Ref Stats:      ", metrics.reference_stats);
    print_image_statistics_text("Cand Stats:     ", metrics.candidate_stats);
}

void print_json_report(std::string_view reference_path,
                       std::string_view candidate_path,
                       const wk::metrics::ImageMetrics& metrics,
                       uintmax_t reference_size,
                       uintmax_t candidate_size,
                       uint8_t bit_depth) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout
        << "{\n"
        << "  \"reference_path\": \"" << json_escape(reference_path) << "\",\n"
        << "  \"candidate_path\": \"" << json_escape(candidate_path) << "\",\n"
        << "  \"width\": " << metrics.width << ",\n"
        << "  \"height\": " << metrics.height << ",\n"
        << "  \"bit_depth\": " << static_cast<unsigned>(bit_depth) << ",\n"
        << "  \"compared_channels\": " << static_cast<unsigned>(metrics.compared_channels) << ",\n"
        << "  \"compared_alpha\": " << (metrics.compared_alpha ? "true" : "false") << ",\n"
        << "  \"reference_bytes\": " << reference_size << ",\n"
        << "  \"candidate_bytes\": " << candidate_size << ",\n"
        << "  \"size_ratio\": "
        << (reference_size > 0 ? static_cast<double>(candidate_size) / static_cast<double>(reference_size) : 0.0)
        << ",\n"
        << "  \"mae\": " << metrics.mae << ",\n"
        << "  \"mse\": " << metrics.mse << ",\n"
        << "  \"psnr\": " << metrics.psnr << ",\n"
        << "  \"ssim\": " << metrics.ssim << ",\n"
        << "  \"channels\": {\n";

    for (uint8_t channel_index = 0; channel_index < metrics.compared_channels; ++channel_index) {
        const auto& channel = metrics.channels[channel_index];
        std::cout
            << "    \"" << channel_label(channel_index) << "\": {"
            << "\"mae\": " << channel.mae << ", "
            << "\"mse\": " << channel.mse << ", "
            << "\"psnr\": " << channel.psnr << ", "
            << "\"ssim\": " << channel.ssim << "}";
        if (channel_index + 1 != metrics.compared_channels) {
            std::cout << ',';
        }
        std::cout << '\n';
    }

    std::cout << "  },\n";
    std::cout << "  \"ycbcr\": {\n";
    for (size_t plane_index = 0; plane_index < metrics.ycbcr.size(); ++plane_index) {
        const auto& plane = metrics.ycbcr[plane_index];
        std::cout
            << "    \"" << plane_label(static_cast<uint8_t>(plane_index)) << "\": {"
            << "\"mae\": " << plane.mae << ", "
            << "\"mse\": " << plane.mse << ", "
            << "\"psnr\": " << plane.psnr << ", "
            << "\"ssim\": " << plane.ssim << "}";
        if (plane_index + 1u != metrics.ycbcr.size()) {
            std::cout << ',';
        }
        std::cout << '\n';
    }

    std::cout
        << "  },\n"
        << "  \"artifacts\": {\n"
        << "    \"chroma_mae\": " << metrics.artifacts.chroma_mae << ",\n"
        << "    \"chroma_mse\": " << metrics.artifacts.chroma_mse << ",\n"
        << "    \"chroma_psnr\": " << metrics.artifacts.chroma_psnr << ",\n"
        << "    \"weighted_luma_mae\": " << metrics.artifacts.weighted_luma_mae << ",\n"
        << "    \"weighted_chroma_mae\": " << metrics.artifacts.weighted_chroma_mae << ",\n"
        << "    \"max_abs_error\": " << metrics.artifacts.max_abs_error << "\n"
        << "  },\n";

    print_image_statistics_json("reference_stats", metrics.reference_stats, true);
    print_image_statistics_json("candidate_stats", metrics.candidate_stats, false);
    std::cout << "}\n";
}

}

int main(int argc, char* argv[]) {
    std::string reference_path;
    std::string candidate_path;
    bool json_output = false;
    bool include_alpha = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            json_output = true;
        } else if (arg == "--rgb-only") {
            include_alpha = false;
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option " << arg << '\n';
            print_usage();
            return 1;
        } else if (reference_path.empty()) {
            reference_path = arg;
        } else if (candidate_path.empty()) {
            candidate_path = arg;
        } else {
            std::cerr << "Error: unexpected extra argument " << arg << '\n';
            return 1;
        }
    }

    if (reference_path.empty() || candidate_path.empty()) {
        print_usage();
        return 1;
    }

    auto reference = load_visual_input(reference_path);
    if (!reference) {
        std::cerr << "Error: failed to load reference: " << reference.error().message << '\n';
        return 1;
    }

    auto candidate = load_visual_input(candidate_path);
    if (!candidate) {
        std::cerr << "Error: failed to load candidate: " << candidate.error().message << '\n';
        return 1;
    }

    auto metrics = wk::metrics::compare_images(*reference, *candidate, include_alpha);
    if (!metrics) {
        std::cerr << "Error: failed to compare images: " << metrics.error().message << '\n';
        return 1;
    }

    const uintmax_t reference_size = file_size_or_zero(reference_path);
    const uintmax_t candidate_size = file_size_or_zero(candidate_path);

    if (json_output) {
        print_json_report(reference_path, candidate_path, *metrics, reference_size, candidate_size,
                          static_cast<uint8_t>(reference->bit_depth()));
    } else {
        print_text_report(reference_path, candidate_path, *metrics, reference_size, candidate_size,
                          static_cast<uint8_t>(reference->bit_depth()));
    }

    return 0;
}