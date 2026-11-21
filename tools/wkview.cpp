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
#include "../src/core/common.h"
#include "../src/core/image_io.h"
#include "dither_display.h"
#include "tonemap.h"
#ifdef WK_VIEWER_GUI
#include <MiniFB.h>
#endif
#include <wk/wk.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

struct DisplayBuffer {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> pixels;
};

struct ViewerPanel {
    DisplayBuffer image;
    // --dither copy of the decoded panel (window only; the terminal
    // path always uses image). Empty when dither is off.
    std::optional<DisplayBuffer> dithered_image;
    std::string label;
    std::vector<std::string> info_lines;
    uint32_t label_bg = 0;
    uint32_t label_text = 0;
    uint32_t info_bg = 0;
    uint32_t info_text = 0;
};

struct ViewerItem {
    std::filesystem::path wk_path;
    std::filesystem::path source_path;
    std::string display_name;
    bool has_source = false;
};

struct ViewerDocument {
    std::filesystem::path wk_path;
    std::optional<wk::ImageInfo> info;
    ViewerPanel decoded_panel;
    std::optional<ViewerPanel> source_panel;
};

struct Rect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    [[nodiscard]] bool contains(int px, int py) const {
        return px >= static_cast<int>(x) &&
               py >= static_cast<int>(y) &&
               px < static_cast<int>(x + width) &&
               py < static_cast<int>(y + height);
    }
};

struct ViewerLayout {
    Rect header;
    Rect body;
    Rect sidebar;
    Rect sidebar_header;
    Rect sidebar_list;
    Rect content;
    Rect image_area;
    Rect details_area;
    Rect footer;
    Rect left_panel;
    Rect right_panel;
    Rect left_label;
    Rect right_label;
    Rect left_image;
    Rect right_image;
    Rect left_info;
    Rect right_info;
    bool has_sidebar = false;
    bool has_compare = false;
    bool has_details = false;
    uint32_t sidebar_row_height = 18;
    uint32_t visible_rows = 0;
};

struct SourceMatcher {
    std::filesystem::path initial_wk;
    std::filesystem::path explicit_file;
    std::vector<std::filesystem::path> candidates;
};

struct ViewerState {
    std::vector<ViewerItem> items;
    size_t current_index = 0;
    bool show_list = true;
    bool show_details = true;
    bool compare_requested = true;
    // Slice 3: --linear disables HDR tone-mapping (linear 16-bit
    // downscale, the pre-slice behaviour) for comparison.
    bool linear_display = false;
    // --dither applies the measured display dither to the decoded
    // panel in the window only (opt-in, default off).
    bool dither_display = false;
    uint32_t list_scroll = 0;
    bool dirty = true;
    unsigned last_window_width = 0;
    unsigned last_window_height = 0;
    ViewerLayout layout;
    DisplayBuffer present;
    ViewerDocument document;
#ifdef WK_VIEWER_GUI
    std::array<uint8_t, static_cast<size_t>(MFB_KB_KEY_LAST) + 1> previous_keys{};
    std::array<uint8_t, 8> previous_mouse_buttons{};
#endif
};

using Glyph = std::array<uint8_t, 7>;

std::string lowercase_ascii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string normalize_match_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    bool previous_underscore = false;
    for (unsigned char ch : text) {
        char out = 0;
        if (std::isalnum(ch) != 0) {
            out = static_cast<char>(std::tolower(ch));
            previous_underscore = false;
        } else {
            if (previous_underscore) {
                continue;
            }
            out = '_';
            previous_underscore = true;
        }
        result.push_back(out);
    }

    while (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }
    return result;
}

std::vector<std::string> split_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : text) {
        if (ch == '_') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string join_tokens(const std::vector<std::string>& tokens, size_t begin, size_t end) {
    std::string joined;
    for (size_t i = begin; i < end; ++i) {
        if (!joined.empty()) {
            joined.push_back('_');
        }
        joined += tokens[i];
    }
    return joined;
}

void add_unique_text(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool is_profile_token(std::string_view token) {
    if (token == "srcjpeg" || token == "yuv444" || token == "yuv420" ||
        token == "lossless" || token == "lossy" || token == "clean") {
        return true;
    }
    if (token.size() >= 2 && token.front() == 'q') {
        return std::all_of(token.begin() + 1, token.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        });
    }
    return false;
}

std::vector<std::string> build_match_keys(std::string_view stem) {
    std::vector<std::string> keys;
    const std::string normalized = normalize_match_text(stem);
    add_unique_text(keys, normalized);

    auto tokens = split_tokens(normalized);
    while (tokens.size() > 1 && is_profile_token(tokens.back())) {
        tokens.pop_back();
    }

    add_unique_text(keys, join_tokens(tokens, 0, tokens.size()));
    if (tokens.size() > 1) {
        add_unique_text(keys, join_tokens(tokens, 1, tokens.size()));
        add_unique_text(keys, join_tokens(tokens, tokens.size() - 1, tokens.size()));
    }
    if (tokens.size() > 2) {
        add_unique_text(keys, join_tokens(tokens, tokens.size() - 2, tokens.size()));
    }

    return keys;
}

int score_match_key(std::string_view key, std::string_view candidate) {
    if (key.empty() || candidate.empty()) {
        return 0;
    }
    if (key == candidate) {
        return 500;
    }
    if (key.size() > candidate.size() && key.find(candidate) != std::string_view::npos) {
        return 280;
    }
    if (candidate.size() > key.size() && candidate.find(key) != std::string_view::npos) {
        return 200;
    }
    const std::string suffix = "_" + std::string(candidate);
    const std::string prefix = std::string(candidate) + "_";
    if (key.ends_with(suffix) || key.starts_with(prefix)) {
        return 340;
    }
    return 0;
}

std::string canonical_key(const std::filesystem::path& path) {
    std::error_code ec;
    const auto weak = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path normalized = ec ? path.lexically_normal() : weak;
    return lowercase_ascii(normalized.generic_string());
}

bool is_wk_file(const std::filesystem::path& path) {
    return lowercase_ascii(path.extension().string()) == ".wk";
}

bool is_source_image_file(const std::filesystem::path& path) {
    const std::string ext = lowercase_ascii(path.extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".ppm";
}

template <typename Predicate>
std::vector<std::filesystem::path> collect_files(const std::filesystem::path& root,
                                                 bool recursive,
                                                 Predicate predicate) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) {
        return files;
    }

    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) {
                continue;
            }
            if (predicate(it->path())) {
                files.push_back(it->path());
            }
        }
    } else {
        for (std::filesystem::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) {
                continue;
            }
            if (predicate(it->path())) {
                files.push_back(it->path());
            }
        }
    }

    std::sort(files.begin(), files.end(), [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
        return lowercase_ascii(lhs.generic_string()) < lowercase_ascii(rhs.generic_string());
    });
    return files;
}

std::vector<std::filesystem::path> suggest_similar_files(std::string_view path) {
    std::filesystem::path requested{std::string(path)};
    std::filesystem::path dir = requested.parent_path().empty() ? std::filesystem::path(".") : requested.parent_path();
    const std::string requested_stem = lowercase_ascii(requested.stem().string());
    const std::string requested_ext = lowercase_ascii(requested.extension().string());

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
        return {};
    }

    struct Candidate {
        int score = 0;
        std::filesystem::path path;
    };

    std::vector<Candidate> candidates;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }

        const std::filesystem::path candidate_path = it->path();
        const std::string candidate_ext = lowercase_ascii(candidate_path.extension().string());
        const std::string candidate_stem = lowercase_ascii(candidate_path.stem().string());
        if (!requested_ext.empty() && candidate_ext != requested_ext) {
            continue;
        }

        int score = 0;
        if (candidate_stem == requested_stem) {
            score += 100;
        }
        if (!requested_stem.empty() && candidate_stem.rfind(requested_stem, 0) == 0) {
            score += 40;
        }
        if (!requested_stem.empty() && candidate_stem.find(requested_stem) != std::string::npos) {
            score += 20;
        }
        if (!requested_stem.empty() && requested_stem.find(candidate_stem) != std::string::npos) {
            score += 10;
        }
        if (candidate_ext == requested_ext) {
            score += 5;
        }

        if (score > 0) {
            candidates.push_back({score, candidate_path});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.path.filename().string().size() < rhs.path.filename().string().size();
    });

    std::vector<std::filesystem::path> results;
    for (const Candidate& candidate : candidates) {
        if (results.size() == 3) {
            break;
        }
        results.push_back(candidate.path);
    }
    return results;
}

std::string make_missing_file_message(std::string_view path) {
    std::string message = "failed to open file: ";
    message += std::string(path);

    const auto suggestions = suggest_similar_files(path);
    if (!suggestions.empty()) {
        message += ". did you mean ";
        for (size_t i = 0; i < suggestions.size(); ++i) {
            if (i > 0) {
                message += i + 1 == suggestions.size() ? " or " : ", ";
            }
            message += suggestions[i].generic_string();
        }
        message += " ?";
    }

    return message;
}

wk::Result<std::vector<uint8_t>> read_file_bytes(std::string_view path) {
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(std::string(path)), ec) || ec) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, make_missing_file_message(path)});
    }

    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to open file: " + std::string(path)});
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to determine file size: " + std::string(path)});
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to read file: " + std::string(path)});
    }
    return bytes;
}

wk::Result<wk::Image> load_image_file(std::string_view path) {
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(std::string(path)), ec) || ec) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, make_missing_file_message(path)});
    }
    return wk::io::load_image_file(path);
}

uintmax_t file_size_or_zero(std::string_view path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(std::filesystem::path(std::string(path)), ec);
    return ec ? 0u : size;
}

uint16_t read_u16_le(const uint8_t* ptr) {
    return static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
}

uint8_t scale_to_u8(uint16_t sample, wk::BitDepth bit_depth) {
    const uint32_t max_value = (1u << static_cast<unsigned>(bit_depth)) - 1u;
    return static_cast<uint8_t>((static_cast<uint32_t>(sample) * 255u + max_value / 2u) / max_value);
}

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Same layout as MiniFB's MFB_ARGB, written out so this file does
    // not need MiniFB for the terminal path.
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint8_t channel_b(uint32_t pixel) {
    return static_cast<uint8_t>(pixel & 0xffu);
}

uint8_t channel_g(uint32_t pixel) {
    return static_cast<uint8_t>((pixel >> 8) & 0xffu);
}

uint8_t channel_r(uint32_t pixel) {
    return static_cast<uint8_t>((pixel >> 16) & 0xffu);
}

uint8_t channel_a(uint32_t pixel) {
    return static_cast<uint8_t>((pixel >> 24) & 0xffu);
}

Glyph glyph_for(char ch) {
    switch (ch) {
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
        case 'J': return {0x1F, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case '1': return {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
        case '6': return {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        case ':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '_': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
        case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        default: return {0x1F, 0x01, 0x02, 0x04, 0x04, 0x00, 0x04};
    }
}

void fill_rect(DisplayBuffer& buffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (x >= buffer.width || y >= buffer.height || width == 0 || height == 0) {
        return;
    }

    const uint32_t max_x = std::min(buffer.width, x + width);
    const uint32_t max_y = std::min(buffer.height, y + height);
    for (uint32_t row = y; row < max_y; ++row) {
        for (uint32_t col = x; col < max_x; ++col) {
            buffer.pixels[static_cast<size_t>(row) * buffer.width + col] = color;
        }
    }
}

void draw_glyph(DisplayBuffer& buffer, uint32_t x, uint32_t y, char ch, uint32_t color) {
    const Glyph glyph = glyph_for(ch);
    for (uint32_t row = 0; row < glyph.size(); ++row) {
        for (uint32_t col = 0; col < 5; ++col) {
            const uint8_t mask = static_cast<uint8_t>(1u << (4u - col));
            if ((glyph[row] & mask) == 0) {
                continue;
            }
            const uint32_t draw_x = x + col;
            const uint32_t draw_y = y + row;
            if (draw_x < buffer.width && draw_y < buffer.height) {
                buffer.pixels[static_cast<size_t>(draw_y) * buffer.width + draw_x] = color;
            }
        }
    }
}

std::string normalize_label(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char ch : text) {
        if (std::isalnum(ch) != 0) {
            result.push_back(static_cast<char>(std::toupper(ch)));
        } else if (ch == '.' || ch == ':' || ch == '-' || ch == '_' || ch == ' ') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back(' ');
        }
    }
    return result;
}

std::string fit_label_text(std::string text, uint32_t panel_width) {
    constexpr uint32_t glyph_width = 6;
    constexpr uint32_t text_padding = 10;
    if (panel_width <= text_padding * 2) {
        return {};
    }

    const size_t max_chars = (panel_width - text_padding * 2) / glyph_width;
    if (max_chars == 0) {
        return {};
    }
    if (text.size() <= max_chars) {
        return text;
    }
    if (max_chars <= 3) {
        return text.substr(0, max_chars);
    }
    return text.substr(0, max_chars - 3) + "...";
}

void draw_text(DisplayBuffer& buffer, uint32_t x, uint32_t y, std::string_view text, uint32_t color) {
    uint32_t cursor_x = x;
    for (char ch : text) {
        draw_glyph(buffer, cursor_x, y, ch, color);
        cursor_x += 6;
        if (cursor_x >= buffer.width) {
            break;
        }
    }
}

DisplayBuffer make_placeholder_image(uint32_t width, uint32_t height, uint32_t background, uint32_t accent) {
    DisplayBuffer image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * height, background);

    fill_rect(image, 0, 0, width, 2, accent);
    fill_rect(image, 0, height >= 2 ? height - 2 : 0, width, std::min(2u, height), accent);
    fill_rect(image, 0, 0, 2, height, accent);
    fill_rect(image, width >= 2 ? width - 2 : 0, 0, std::min(2u, width), height, accent);

    for (uint32_t i = 0; i < std::min(width, height); ++i) {
        image.pixels[static_cast<size_t>(i) * width + i] = accent;
        image.pixels[static_cast<size_t>(height - 1 - i) * width + i] = accent;
    }
    return image;
}

DisplayBuffer rasterize(const wk::Image& image, uint8_t cicp_transfer, bool linear_display) {
    DisplayBuffer display;
    display.width = image.width();
    display.height = image.height();
    display.pixels.resize(static_cast<size_t>(display.width) * display.height);

    const auto source = image.pixels();
    const bool has_alpha = image.has_alpha();

    if (image.bit_depth() == wk::BitDepth::Bits8) {
        // SDR 8-bit: bytes are copied directly, never tone-mapped.
        const size_t bytes_per_pixel = has_alpha ? 4u : 3u;
        for (size_t i = 0; i < display.pixels.size(); ++i) {
            const uint8_t* pixel = source.data() + i * bytes_per_pixel;
            const uint8_t alpha = has_alpha ? pixel[3] : 255;
            display.pixels[i] = pack_rgba(pixel[0], pixel[1], pixel[2], alpha);
        }
        return display;
    }

    const size_t bytes_per_pixel = has_alpha ? 8u : 6u;
    const uint16_t max_value =
        static_cast<uint16_t>((1u << static_cast<unsigned>(image.bit_depth())) - 1u);

    if (!linear_display && wkview::is_hdr_transfer(cicp_transfer)) {
        // Slice 3 tone-map (viewer-only): PQ/HLG -> linear light via
        // the EOTF -> Reinhard with a geometric-mean key value ->
        // sRGB OETF -> 8-bit. The key value needs one pre-pass over
        // the frame's linear luminance. Alpha stays linear, it is
        // not light.
        std::vector<double> luminance(display.pixels.size());
        for (size_t i = 0; i < display.pixels.size(); ++i) {
            const uint8_t* pixel = source.data() + i * bytes_per_pixel;
            const double r = wkview::tonemap_linear_rel(read_u16_le(pixel + 0), max_value,
                                                        cicp_transfer);
            const double g = wkview::tonemap_linear_rel(read_u16_le(pixel + 2), max_value,
                                                        cicp_transfer);
            const double b = wkview::tonemap_linear_rel(read_u16_le(pixel + 4), max_value,
                                                        cicp_transfer);
            luminance[i] = wkview::tonemap_luminance(r, g, b);
        }
        const double key_scale =
            wkview::tonemap_key_scale(wkview::tonemap_geometric_mean(luminance.data(),
                                                                     luminance.size()));
        for (size_t i = 0; i < display.pixels.size(); ++i) {
            const uint8_t* pixel = source.data() + i * bytes_per_pixel;
            const uint8_t r = wkview::tonemap_to_u8(read_u16_le(pixel + 0), max_value,
                                                    cicp_transfer, key_scale);
            const uint8_t g = wkview::tonemap_to_u8(read_u16_le(pixel + 2), max_value,
                                                    cicp_transfer, key_scale);
            const uint8_t b = wkview::tonemap_to_u8(read_u16_le(pixel + 4), max_value,
                                                    cicp_transfer, key_scale);
            const uint8_t a = has_alpha ? scale_to_u8(read_u16_le(pixel + 6), image.bit_depth())
                                        : 255;
            display.pixels[i] = pack_rgba(r, g, b, a);
        }
        return display;
    }

    for (size_t i = 0; i < display.pixels.size(); ++i) {
        const uint8_t* pixel = source.data() + i * bytes_per_pixel;
        const uint8_t r = scale_to_u8(read_u16_le(pixel + 0), image.bit_depth());
        const uint8_t g = scale_to_u8(read_u16_le(pixel + 2), image.bit_depth());
        const uint8_t b = scale_to_u8(read_u16_le(pixel + 4), image.bit_depth());
        const uint8_t a = has_alpha ? scale_to_u8(read_u16_le(pixel + 6), image.bit_depth()) : 255;
        display.pixels[i] = pack_rgba(r, g, b, a);
    }
    return display;
}

// Dithered copy of a rasterized panel for the window only. The plain
// image is left untouched (the terminal path and the source panel
// always use it).
DisplayBuffer make_dithered_copy(const DisplayBuffer& image, uint8_t cicp_matrix, bool full_range) {
    DisplayBuffer out = image;
    std::vector<uint8_t> rgb(image.pixels.size() * 3);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const uint32_t p = image.pixels[i];
        rgb[i * 3 + 0] = static_cast<uint8_t>((p >> 16) & 0xff);
        rgb[i * 3 + 1] = static_cast<uint8_t>((p >> 8) & 0xff);
        rgb[i * 3 + 2] = static_cast<uint8_t>(p & 0xff);
    }
    wkview::apply_display_dither(rgb.data(), image.width, image.height, cicp_matrix, full_range);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const uint32_t p = image.pixels[i];
        out.pixels[i] = pack_rgba(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2],
                                  static_cast<uint8_t>((p >> 24) & 0xff));
    }
    return out;
}

void blit_image(DisplayBuffer& canvas, const DisplayBuffer& image, uint32_t x, uint32_t y) {
    for (uint32_t row = 0; row < image.height; ++row) {
        std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(row) * image.width,
                    image.width,
                    canvas.pixels.begin() + static_cast<std::ptrdiff_t>(y + row) * canvas.width + x);
    }
}

std::string bool_text(bool value) {
    return value ? "YES" : "NO";
}

std::string format_size_human(uintmax_t bytes) {
    static constexpr std::array<std::string_view, 4> units = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index + 1 < units.size()) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream stream;
    if (unit_index == 0) {
        stream << bytes << ' ' << units[unit_index];
    } else {
        stream << std::fixed << std::setprecision(value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2));
        stream << value << ' ' << units[unit_index];
    }
    return stream.str();
}

std::string file_extension_label(std::string_view path) {
    std::string ext = std::filesystem::path(std::string(path)).extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    if (ext.empty()) {
        return "FILE";
    }
    return normalize_label(ext);
}

std::vector<std::string> make_wk_info_lines(std::string_view path, uintmax_t bytes, const wk::ImageInfo& info) {
    return {
        "FILE: " + std::filesystem::path(std::string(path)).filename().string(),
        "SIZE: " + std::to_string(bytes) + " B " + format_size_human(bytes),
        "DIM: " + std::to_string(info.width) + "X" + std::to_string(info.height),
        "DEPTH: " + std::to_string(static_cast<unsigned>(info.bit_depth)) + " BIT",
        "MODE: " + std::string(info.is_lossless ? "LOSSLESS" : "LOSSY"),
        "ALPHA: " + bool_text(info.has_alpha),
        "WKMETA: " + bool_text(info.has_wkmeta),
        "HDR: " + bool_text(info.is_hdr),
        "TILE: " + std::to_string(info.tile_size) + " PX",
        "FRAMES: " + std::to_string(info.frame_count),
        "CICP: P" + std::to_string(info.cicp.primaries) + " T" + std::to_string(info.cicp.transfer) +
            " M" + std::to_string(info.cicp.matrix) + " " + (info.cicp.full_range ? std::string("FULL") : std::string("LIMITED"))
    };
}

std::vector<std::string> make_source_info_lines(std::string_view path, uintmax_t bytes, const wk::Image& image) {
    return {
        "FILE: " + std::filesystem::path(std::string(path)).filename().string(),
        "SIZE: " + std::to_string(bytes) + " B " + format_size_human(bytes),
        "FORMAT: " + file_extension_label(path),
        "DIM: " + std::to_string(image.width()) + "X" + std::to_string(image.height()),
        "DEPTH: " + std::to_string(static_cast<unsigned>(image.bit_depth())) + " BIT",
        "ALPHA: " + bool_text(image.has_alpha())
    };
}

std::vector<std::string> make_error_lines(std::string_view path, std::string_view message) {
    return {
        "FILE: " + std::filesystem::path(std::string(path)).filename().string(),
        "STATUS: ERROR",
        "MESSAGE: " + std::string(message)
    };
}

ViewerPanel make_message_panel(std::string label,
                               std::vector<std::string> info_lines,
                               uint32_t label_bg,
                               uint32_t label_text,
                               uint32_t info_bg,
                               uint32_t info_text,
                               uint32_t image_bg,
                               uint32_t image_accent) {
    ViewerPanel panel;
    panel.image = make_placeholder_image(256, 160, image_bg, image_accent);
    panel.label = std::move(label);
    panel.info_lines = std::move(info_lines);
    panel.label_bg = label_bg;
    panel.label_text = label_text;
    panel.info_bg = info_bg;
    panel.info_text = info_text;
    return panel;
}

uint32_t info_block_height(const ViewerPanel& panel) {
    constexpr uint32_t line_height = 10;
    constexpr uint32_t padding_y = 8;
    return padding_y * 2 + static_cast<uint32_t>(panel.info_lines.size()) * line_height;
}

void draw_info_block(DisplayBuffer& canvas, const ViewerPanel& panel, const Rect& rect) {
    if (rect.width == 0 || rect.height == 0) {
        return;
    }

    fill_rect(canvas, rect.x, rect.y, rect.width, rect.height, panel.info_bg);
    constexpr uint32_t line_height = 10;
    constexpr uint32_t padding_x = 10;
    constexpr uint32_t padding_y = 8;

    uint32_t cursor_y = rect.y + padding_y;
    for (const std::string& line : panel.info_lines) {
        if (cursor_y + 7 > rect.y + rect.height) {
            break;
        }
        draw_text(canvas,
                  rect.x + padding_x,
                  cursor_y,
                  fit_label_text(normalize_label(line), rect.width),
                  panel.info_text);
        cursor_y += line_height;
    }
}

struct FitRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

FitRect best_fit_rect(uint32_t src_width, uint32_t src_height, uint32_t dst_width, uint32_t dst_height) {
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
        return {};
    }

    const double scale = std::min(static_cast<double>(dst_width) / static_cast<double>(src_width),
                                  static_cast<double>(dst_height) / static_cast<double>(src_height));
    const uint32_t fit_width = std::max(1u, static_cast<uint32_t>(std::lround(src_width * scale)));
    const uint32_t fit_height = std::max(1u, static_cast<uint32_t>(std::lround(src_height * scale)));

    FitRect rect;
    rect.width = std::min(fit_width, dst_width);
    rect.height = std::min(fit_height, dst_height);
    rect.x = (dst_width - rect.width) / 2u;
    rect.y = (dst_height - rect.height) / 2u;
    return rect;
}

uint32_t sample_bilinear(const DisplayBuffer& source, double src_x, double src_y) {
    const double clamped_x = std::clamp(src_x, 0.0, static_cast<double>(source.width - 1));
    const double clamped_y = std::clamp(src_y, 0.0, static_cast<double>(source.height - 1));
    const uint32_t x0 = static_cast<uint32_t>(clamped_x);
    const uint32_t y0 = static_cast<uint32_t>(clamped_y);
    const uint32_t x1 = std::min(x0 + 1, source.width - 1);
    const uint32_t y1 = std::min(y0 + 1, source.height - 1);
    const double tx = clamped_x - static_cast<double>(x0);
    const double ty = clamped_y - static_cast<double>(y0);

    const uint32_t p00 = source.pixels[static_cast<size_t>(y0) * source.width + x0];
    const uint32_t p10 = source.pixels[static_cast<size_t>(y0) * source.width + x1];
    const uint32_t p01 = source.pixels[static_cast<size_t>(y1) * source.width + x0];
    const uint32_t p11 = source.pixels[static_cast<size_t>(y1) * source.width + x1];

    auto interpolate_channel = [&](uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11) -> uint8_t {
        const double top = static_cast<double>(c00) + (static_cast<double>(c10) - static_cast<double>(c00)) * tx;
        const double bottom = static_cast<double>(c01) + (static_cast<double>(c11) - static_cast<double>(c01)) * tx;
        return static_cast<uint8_t>(std::clamp(std::lround(top + (bottom - top) * ty), 0l, 255l));
    };

    return pack_rgba(
        interpolate_channel(channel_r(p00), channel_r(p10), channel_r(p01), channel_r(p11)),
        interpolate_channel(channel_g(p00), channel_g(p10), channel_g(p01), channel_g(p11)),
        interpolate_channel(channel_b(p00), channel_b(p10), channel_b(p01), channel_b(p11)),
        interpolate_channel(channel_a(p00), channel_a(p10), channel_a(p01), channel_a(p11)));
}

void draw_scaled_image(DisplayBuffer& canvas,
                       const DisplayBuffer& image,
                       const Rect& rect,
                       uint32_t background) {
    if (rect.width == 0 || rect.height == 0) {
        return;
    }

    fill_rect(canvas, rect.x, rect.y, rect.width, rect.height, background);
    if (image.width == 0 || image.height == 0) {
        return;
    }

    const FitRect fit = best_fit_rect(image.width, image.height, rect.width, rect.height);
    if (fit.width == 0 || fit.height == 0) {
        return;
    }

    if (fit.width == image.width && fit.height == image.height) {
        blit_image(canvas, image, rect.x + fit.x, rect.y + fit.y);
        return;
    }

    const bool shrinking = fit.width < image.width || fit.height < image.height;
    for (uint32_t dst_y = 0; dst_y < fit.height; ++dst_y) {
        for (uint32_t dst_x = 0; dst_x < fit.width; ++dst_x) {
            const double src_x = (static_cast<double>(dst_x) + 0.5) * static_cast<double>(image.width) / static_cast<double>(fit.width) - 0.5;
            const double src_y = (static_cast<double>(dst_y) + 0.5) * static_cast<double>(image.height) / static_cast<double>(fit.height) - 0.5;

            uint32_t pixel = 0;
            if (shrinking) {
                pixel = sample_bilinear(image, src_x, src_y);
            } else {
                const uint32_t nearest_x = std::min(static_cast<uint32_t>(std::lround(std::clamp(src_x, 0.0, static_cast<double>(image.width - 1)))), image.width - 1);
                const uint32_t nearest_y = std::min(static_cast<uint32_t>(std::lround(std::clamp(src_y, 0.0, static_cast<double>(image.height - 1)))), image.height - 1);
                pixel = image.pixels[static_cast<size_t>(nearest_y) * image.width + nearest_x];
            }

            const uint32_t out_x = rect.x + fit.x + dst_x;
            const uint32_t out_y = rect.y + fit.y + dst_y;
            if (out_x < canvas.width && out_y < canvas.height) {
                canvas.pixels[static_cast<size_t>(out_y) * canvas.width + out_x] = pixel;
            }
        }
    }
}

std::string make_playlist_label(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    std::string label = ec || relative.empty() ? path.filename().string() : relative.generic_string();
    std::replace(label.begin(), label.end(), '/', ' ');
    std::replace(label.begin(), label.end(), '\\', ' ');
    if (label.empty()) {
        label = path.filename().string();
    }
    return label;
}

SourceMatcher build_source_matcher(std::string_view source_hint, const std::filesystem::path& initial_wk) {
    SourceMatcher matcher;
    matcher.initial_wk = initial_wk;

    std::filesystem::path hint_path;
    if (!source_hint.empty()) {
        hint_path = std::filesystem::path(std::string(source_hint));
    }

    std::error_code ec;
    if (!hint_path.empty() && std::filesystem::is_regular_file(hint_path, ec) && !ec && is_source_image_file(hint_path)) {
        matcher.explicit_file = hint_path;
        matcher.candidates = collect_files(hint_path.parent_path(), false, is_source_image_file);
        return matcher;
    }

    std::filesystem::path search_root = hint_path;
    if (search_root.empty()) {
        search_root = initial_wk.parent_path().empty() ? std::filesystem::path(".") : initial_wk.parent_path();
    }

    if (std::filesystem::is_directory(search_root, ec) && !ec) {
        matcher.candidates = collect_files(search_root, false, is_source_image_file);
        if (matcher.candidates.empty()) {
            matcher.candidates = collect_files(search_root, true, is_source_image_file);
        }
    }

    return matcher;
}

std::optional<std::filesystem::path> resolve_source_path(const std::filesystem::path& wk_path,
                                                         const SourceMatcher& matcher) {
    if (!matcher.explicit_file.empty() && canonical_key(wk_path) == canonical_key(matcher.initial_wk)) {
        return matcher.explicit_file;
    }
    if (matcher.candidates.empty()) {
        return std::nullopt;
    }

    const std::vector<std::string> match_keys = build_match_keys(wk_path.stem().string());

    int best_score = 0;
    std::optional<std::filesystem::path> best_path;
    for (const auto& candidate : matcher.candidates) {
        const std::string candidate_key = normalize_match_text(candidate.stem().string());
        int score = 0;
        for (const std::string& key : match_keys) {
            score = std::max(score, score_match_key(key, candidate_key));
        }

        const std::string ext = lowercase_ascii(candidate.extension().string());
        if (ext == ".jpg" || ext == ".jpeg") {
            score += 8;
        } else if (ext == ".png") {
            score += 4;
        }

        if (score > best_score) {
            best_score = score;
            best_path = candidate;
        }
    }

    if (best_score <= 0) {
        return std::nullopt;
    }
    return best_path;
}

wk::Result<std::pair<std::vector<ViewerItem>, size_t>> build_playlist(std::string_view input_path,
                                                                      std::string_view source_hint) {
    std::filesystem::path requested{std::string(input_path)};
    std::error_code ec;
    if (!std::filesystem::exists(requested, ec) || ec) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, make_missing_file_message(input_path)});
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::path list_root;
    size_t current_index = 0;

    if (std::filesystem::is_regular_file(requested, ec) && !ec) {
        if (!is_wk_file(requested)) {
            return std::unexpected(wk::Error{wk::ErrorCode::InvalidParameter, "wkview expects a .wk file or a directory of .wk files"});
        }
        list_root = requested.parent_path().empty() ? std::filesystem::path(".") : requested.parent_path();
        files = collect_files(list_root, false, is_wk_file);
        if (files.empty()) {
            files.push_back(requested);
        }

        const std::string requested_key = canonical_key(requested);
        for (size_t i = 0; i < files.size(); ++i) {
            if (canonical_key(files[i]) == requested_key) {
                current_index = i;
                break;
            }
        }
    } else if (std::filesystem::is_directory(requested, ec) && !ec) {
        list_root = requested;
        files = collect_files(requested, false, is_wk_file);
        if (files.empty()) {
            files = collect_files(requested, true, is_wk_file);
        }
        if (files.empty()) {
            return std::unexpected(wk::Error{wk::ErrorCode::IoError, "no .wk files found in directory: " + requested.generic_string()});
        }
    } else {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "unsupported input path: " + requested.generic_string()});
    }

    SourceMatcher matcher = build_source_matcher(source_hint, files[current_index]);
    std::vector<ViewerItem> items;
    items.reserve(files.size());

    for (const auto& file : files) {
        ViewerItem item;
        item.wk_path = file;
        item.display_name = make_playlist_label(file, list_root);
        auto source = resolve_source_path(file, matcher);
        if (source) {
            item.source_path = *source;
            item.has_source = true;
        }
        items.push_back(std::move(item));
    }

    return std::make_pair(std::move(items), current_index);
}

ViewerDocument load_document(const ViewerItem& item, bool linear_display, bool dither_display) {
    ViewerDocument document;
    document.wk_path = item.wk_path;

    auto wk_bytes = read_file_bytes(item.wk_path.generic_string());
    if (!wk_bytes) {
        document.decoded_panel = make_message_panel(
            "WK: " + item.wk_path.filename().string(),
            make_error_lines(item.wk_path.generic_string(), wk_bytes.error().message),
            pack_rgba(160, 64, 64, 255),
            pack_rgba(255, 240, 240, 255),
            pack_rgba(48, 24, 24, 255),
            pack_rgba(255, 220, 220, 255),
            pack_rgba(24, 12, 12, 255),
            pack_rgba(210, 90, 90, 255));
        return document;
    }

    auto info = wk::get_info(*wk_bytes);
    if (!info) {
        document.decoded_panel = make_message_panel(
            "WK: " + item.wk_path.filename().string(),
            make_error_lines(item.wk_path.generic_string(), info.error().message),
            pack_rgba(160, 64, 64, 255),
            pack_rgba(255, 240, 240, 255),
            pack_rgba(48, 24, 24, 255),
            pack_rgba(255, 220, 220, 255),
            pack_rgba(24, 12, 12, 255),
            pack_rgba(210, 90, 90, 255));
        return document;
    }

    auto decoded = wk::decode(*wk_bytes);
    if (!decoded) {
        document.decoded_panel = make_message_panel(
            "WK: " + item.wk_path.filename().string(),
            make_error_lines(item.wk_path.generic_string(), decoded.error().message),
            pack_rgba(160, 64, 64, 255),
            pack_rgba(255, 240, 240, 255),
            pack_rgba(48, 24, 24, 255),
            pack_rgba(255, 220, 220, 255),
            pack_rgba(24, 12, 12, 255),
            pack_rgba(210, 90, 90, 255));
        return document;
    }

    document.info = *info;
    // The decoded wk::Image does not carry CICP (its constructor
    // defaults it), so the transfer comes from the FHDR header read
    // above. No public struct changes for this.
    document.decoded_panel.image = rasterize(*decoded, info->cicp.transfer, linear_display);
    if (dither_display) {
        document.decoded_panel.dithered_image = make_dithered_copy(
            document.decoded_panel.image, info->cicp.matrix, info->cicp.full_range);
    }
    document.decoded_panel.label = "WK: " + item.wk_path.filename().string();
    document.decoded_panel.info_lines = make_wk_info_lines(item.wk_path.generic_string(),
                                                           file_size_or_zero(item.wk_path.generic_string()),
                                                           *info);
    document.decoded_panel.label_bg = pack_rgba(168, 112, 24, 255);
    document.decoded_panel.label_text = pack_rgba(255, 245, 220, 255);
    document.decoded_panel.info_bg = pack_rgba(44, 34, 18, 255);
    document.decoded_panel.info_text = pack_rgba(255, 236, 196, 255);

    if (!item.has_source) {
        return document;
    }

    auto source_image = load_image_file(item.source_path.generic_string());
    if (!source_image) {
        document.source_panel = make_message_panel(
            "SOURCE: " + item.source_path.filename().string(),
            make_error_lines(item.source_path.generic_string(), source_image.error().message),
            pack_rgba(88, 72, 132, 255),
            pack_rgba(245, 238, 255, 255),
            pack_rgba(26, 22, 44, 255),
            pack_rgba(235, 228, 255, 255),
            pack_rgba(15, 12, 24, 255),
            pack_rgba(136, 120, 200, 255));
        return document;
    }

    ViewerPanel source_panel;
    // A source file carries no CICP (PPM has none; stb loaders do not
    // set it), so its own transfer is BT.709 by default and a 16-bit
    // source stays linear, as before. Only the decoded panel is
    // tone-mapped, from the FHDR transfer.
    source_panel.image = rasterize(*source_image, source_image->info().cicp.transfer,
                                   linear_display);
    source_panel.label = "SOURCE: " + item.source_path.filename().string();
    source_panel.info_lines = make_source_info_lines(item.source_path.generic_string(),
                                                     file_size_or_zero(item.source_path.generic_string()),
                                                     *source_image);
    source_panel.label_bg = pack_rgba(26, 100, 128, 255);
    source_panel.label_text = pack_rgba(232, 248, 255, 255);
    source_panel.info_bg = pack_rgba(18, 39, 54, 255);
    source_panel.info_text = pack_rgba(220, 244, 255, 255);
    document.source_panel = std::move(source_panel);

    return document;
}

#ifdef WK_VIEWER_GUI

std::string build_window_title(const ViewerState& state) {
    const ViewerItem& item = state.items[state.current_index];
    std::string title = "WK Viewer - ";
    title += item.wk_path.filename().string();
    title += " - ";
    title += std::to_string(state.current_index + 1);
    title += " of ";
    title += std::to_string(state.items.size());

    if (state.document.info) {
        title += " - ";
        title += std::to_string(state.document.info->width);
        title += 'x';
        title += std::to_string(state.document.info->height);
        title += " - ";
        title += state.document.info->is_lossless ? "LOSSLESS" : "LOSSY";
    } else {
        title += " - ERROR";
    }

    if (state.compare_requested && state.document.source_panel.has_value()) {
        title += " - compare ";
        title += state.document.source_panel->label.substr(8);
    }

    return title;
}

std::string make_header_text(const ViewerState& state) {
    const ViewerItem& item = state.items[state.current_index];
    std::string text = std::to_string(state.current_index + 1);
    text += " OF ";
    text += std::to_string(state.items.size());
    text += "  ";
    text += item.display_name;
    if (state.document.info) {
        text += "  ";
        text += state.document.info->is_lossless ? "LOSSLESS" : "LOSSY";
        text += "  ";
        text += std::to_string(state.document.info->width);
        text += "X";
        text += std::to_string(state.document.info->height);
    }
    return text;
}

std::string make_footer_text(const ViewerState& state) {
    std::string text = "LEFT RIGHT NAV  UP DOWN LIST  CLICK SELECT  I DETAILS  L LIBRARY  TAB COMPARE  ESC EXIT";
    if (!state.document.source_panel.has_value()) {
        text += "  SOURCE NONE";
    } else if (!state.compare_requested) {
        text += "  COMPARE OFF";
    } else {
        text += "  COMPARE ON";
    }
    return text;
}

ViewerLayout compute_layout(const ViewerState& state, uint32_t window_width, uint32_t window_height) {
    ViewerLayout layout;
    constexpr uint32_t outer_padding = 10;
    constexpr uint32_t section_gap = 12;
    constexpr uint32_t header_height = 28;
    constexpr uint32_t footer_height = 24;
    constexpr uint32_t panel_gap = 16;
    constexpr uint32_t label_height = 24;
    constexpr uint32_t min_image_height = 170;

    const uint32_t safe_width = std::max(1u, window_width);
    const uint32_t safe_height = std::max(1u, window_height);

    layout.header = {outer_padding, outer_padding, safe_width > outer_padding * 2 ? safe_width - outer_padding * 2 : 1u, header_height};
    layout.footer = {outer_padding,
                     safe_height > outer_padding + footer_height ? safe_height - outer_padding - footer_height : 0u,
                     layout.header.width,
                     footer_height};

    const uint32_t body_y = layout.header.y + layout.header.height + section_gap;
    const uint32_t body_height = layout.footer.y > body_y + section_gap ? layout.footer.y - body_y - section_gap : 1u;
    layout.body = {outer_padding, body_y, layout.header.width, body_height};

    layout.has_sidebar = state.show_list && state.items.size() > 1 && layout.body.width >= 860u;
    uint32_t sidebar_width = 0;
    if (layout.has_sidebar) {
        sidebar_width = std::clamp(layout.body.width / 4u, 220u, 320u);
        layout.sidebar = {layout.body.x, layout.body.y, sidebar_width, layout.body.height};
        layout.sidebar_header = {layout.sidebar.x, layout.sidebar.y, layout.sidebar.width, 24u};
        const uint32_t sidebar_list_y = layout.sidebar_header.y + layout.sidebar_header.height + 8u;
        const uint32_t sidebar_list_height = layout.sidebar.height > 36u ? layout.sidebar.height - 36u : 1u;
        layout.sidebar_list = {layout.sidebar.x + 6u, sidebar_list_y, layout.sidebar.width > 12u ? layout.sidebar.width - 12u : 1u, sidebar_list_height};
        layout.visible_rows = std::max(1u, layout.sidebar_list.height / layout.sidebar_row_height);
    }

    layout.content = layout.body;
    if (layout.has_sidebar) {
        const uint32_t content_x = layout.sidebar.x + layout.sidebar.width + section_gap;
        layout.content = {
            content_x,
            layout.body.y,
            layout.body.x + layout.body.width > content_x ? layout.body.x + layout.body.width - content_x : 1u,
            layout.body.height
        };
    }

    layout.has_compare = state.compare_requested && state.document.source_panel.has_value();
    const uint32_t target_details_height = std::clamp<uint32_t>(
        std::max(info_block_height(state.document.decoded_panel),
                 layout.has_compare ? info_block_height(*state.document.source_panel) : 0u),
        84u,
        140u);

    layout.has_details = state.show_details && layout.content.height > min_image_height + target_details_height + section_gap;
    const uint32_t details_height = layout.has_details ? target_details_height : 0u;
    if (layout.has_details) {
        layout.details_area = {
            layout.content.x,
            layout.content.y + layout.content.height - details_height,
            layout.content.width,
            details_height
        };
    }

    const uint32_t image_height = layout.has_details
        ? layout.content.height - details_height - section_gap
        : layout.content.height;
    layout.image_area = {layout.content.x, layout.content.y, layout.content.width, std::max(1u, image_height)};

    const uint32_t left_width = layout.has_compare
        ? std::max(1u, (layout.image_area.width > panel_gap ? (layout.image_area.width - panel_gap) / 2u : layout.image_area.width))
        : layout.image_area.width;
    const uint32_t right_width = layout.has_compare && layout.image_area.width > left_width + panel_gap
        ? layout.image_area.width - left_width - panel_gap
        : 0u;

    layout.left_panel = {layout.image_area.x, layout.image_area.y, left_width, layout.image_area.height};
    layout.left_label = {layout.left_panel.x, layout.left_panel.y, layout.left_panel.width, std::min(label_height, layout.left_panel.height)};
    layout.left_image = {layout.left_panel.x,
                         layout.left_panel.y + layout.left_label.height,
                         layout.left_panel.width,
                         layout.left_panel.height > layout.left_label.height ? layout.left_panel.height - layout.left_label.height : 1u};

    if (layout.has_compare) {
        layout.right_panel = {
            layout.left_panel.x + layout.left_panel.width + panel_gap,
            layout.image_area.y,
            right_width,
            layout.image_area.height
        };
        layout.right_label = {layout.right_panel.x, layout.right_panel.y, layout.right_panel.width, std::min(label_height, layout.right_panel.height)};
        layout.right_image = {layout.right_panel.x,
                              layout.right_panel.y + layout.right_label.height,
                              layout.right_panel.width,
                              layout.right_panel.height > layout.right_label.height ? layout.right_panel.height - layout.right_label.height : 1u};
    }

    if (layout.has_details) {
        if (layout.has_compare) {
            layout.left_info = {layout.details_area.x, layout.details_area.y, left_width, layout.details_area.height};
            layout.right_info = {layout.right_panel.x, layout.details_area.y, right_width, layout.details_area.height};
        } else {
            layout.left_info = layout.details_area;
        }
    }

    return layout;
}

void draw_sidebar(DisplayBuffer& canvas, const ViewerState& state, const ViewerLayout& layout) {
    if (!layout.has_sidebar) {
        return;
    }

    fill_rect(canvas, layout.sidebar.x, layout.sidebar.y, layout.sidebar.width, layout.sidebar.height, pack_rgba(18, 22, 28, 255));
    fill_rect(canvas, layout.sidebar_header.x, layout.sidebar_header.y, layout.sidebar_header.width, layout.sidebar_header.height, pack_rgba(34, 44, 56, 255));
    draw_text(canvas,
              layout.sidebar_header.x + 10,
              layout.sidebar_header.y + 8,
              fit_label_text("FILES " + std::to_string(state.current_index + 1) + " OF " + std::to_string(state.items.size()),
                             layout.sidebar_header.width),
              pack_rgba(224, 236, 248, 255));

    const size_t max_scroll = state.items.size() > layout.visible_rows
        ? state.items.size() - layout.visible_rows
        : 0u;
    const size_t scroll = std::min<size_t>(state.list_scroll, max_scroll);
    const uint32_t marker_size = 6;

    for (size_t row = 0; row < layout.visible_rows; ++row) {
        const size_t item_index = scroll + row;
        if (item_index >= state.items.size()) {
            break;
        }

        const uint32_t row_y = layout.sidebar_list.y + static_cast<uint32_t>(row) * layout.sidebar_row_height;
        const Rect row_rect{layout.sidebar_list.x, row_y, layout.sidebar_list.width, layout.sidebar_row_height - 2u};
        const bool selected = item_index == state.current_index;

        fill_rect(canvas,
                  row_rect.x,
                  row_rect.y,
                  row_rect.width,
                  row_rect.height,
                  selected ? pack_rgba(68, 88, 112, 255) : pack_rgba(24, 30, 38, 255));

        if (state.items[item_index].has_source) {
            fill_rect(canvas,
                      row_rect.x + row_rect.width - 12,
                      row_rect.y + (row_rect.height - marker_size) / 2u,
                      marker_size,
                      marker_size,
                      selected ? pack_rgba(246, 212, 128, 255) : pack_rgba(74, 196, 212, 255));
        }

        draw_text(canvas,
                  row_rect.x + 8,
                  row_rect.y + 5,
                  fit_label_text(normalize_label(state.items[item_index].display_name), row_rect.width - 20),
                  selected ? pack_rgba(250, 248, 242, 255) : pack_rgba(204, 214, 224, 255));
    }
}

void draw_panel(DisplayBuffer& canvas,
                const ViewerPanel& panel,
                const DisplayBuffer& image,
                const Rect& label_rect,
                const Rect& image_rect,
                uint32_t image_bg) {
    if (label_rect.width > 0 && label_rect.height > 0) {
        fill_rect(canvas, label_rect.x, label_rect.y, label_rect.width, label_rect.height, panel.label_bg);
        draw_text(canvas,
                  label_rect.x + 10,
                  label_rect.y + 8,
                  fit_label_text(normalize_label(panel.label), label_rect.width),
                  panel.label_text);
    }
    draw_scaled_image(canvas, image, image_rect, image_bg);
}

DisplayBuffer compose_window(const ViewerState& state, uint32_t window_width, uint32_t window_height, ViewerLayout& out_layout) {
    out_layout = compute_layout(state, window_width, window_height);

    DisplayBuffer canvas;
    canvas.width = std::max(1u, window_width);
    canvas.height = std::max(1u, window_height);
    canvas.pixels.assign(static_cast<size_t>(canvas.width) * canvas.height, pack_rgba(22, 22, 24, 255));

    fill_rect(canvas, out_layout.header.x, out_layout.header.y, out_layout.header.width, out_layout.header.height, pack_rgba(38, 42, 48, 255));
    draw_text(canvas,
              out_layout.header.x + 10,
              out_layout.header.y + 9,
              fit_label_text(normalize_label(make_header_text(state)), out_layout.header.width),
              pack_rgba(236, 240, 244, 255));

    fill_rect(canvas, out_layout.footer.x, out_layout.footer.y, out_layout.footer.width, out_layout.footer.height, pack_rgba(34, 34, 38, 255));
    draw_text(canvas,
              out_layout.footer.x + 10,
              out_layout.footer.y + 8,
              fit_label_text(normalize_label(make_footer_text(state)), out_layout.footer.width),
              pack_rgba(198, 208, 220, 255));

    draw_sidebar(canvas, state, out_layout);

    const uint32_t image_bg = pack_rgba(10, 10, 10, 255);
    draw_panel(canvas, state.document.decoded_panel,
               state.document.decoded_panel.dithered_image.has_value()
                   ? *state.document.decoded_panel.dithered_image
                   : state.document.decoded_panel.image,
               out_layout.left_label, out_layout.left_image, image_bg);
    if (out_layout.has_compare && state.document.source_panel.has_value()) {
        draw_panel(canvas, *state.document.source_panel, state.document.source_panel->image,
                   out_layout.right_label, out_layout.right_image, image_bg);
        fill_rect(canvas,
                  out_layout.left_panel.x + out_layout.left_panel.width,
                  out_layout.image_area.y,
                  16u,
                  out_layout.image_area.height,
                  pack_rgba(54, 54, 58, 255));
    }

    if (out_layout.has_details) {
        draw_info_block(canvas, state.document.decoded_panel, out_layout.left_info);
        if (out_layout.has_compare && state.document.source_panel.has_value()) {
            draw_info_block(canvas, *state.document.source_panel, out_layout.right_info);
        }
    }

    return canvas;
}

#endif  // WK_VIEWER_GUI

void clamp_scroll(ViewerState& state) {
    if (!state.layout.has_sidebar || state.layout.visible_rows == 0) {
        state.list_scroll = 0;
        return;
    }
    const size_t max_scroll = state.items.size() > state.layout.visible_rows
        ? state.items.size() - state.layout.visible_rows
        : 0u;
    state.list_scroll = static_cast<uint32_t>(std::min<size_t>(state.list_scroll, max_scroll));
}

void ensure_selection_visible(ViewerState& state) {
    if (!state.layout.has_sidebar || state.layout.visible_rows == 0) {
        state.list_scroll = 0;
        return;
    }

    if (state.current_index < state.list_scroll) {
        state.list_scroll = static_cast<uint32_t>(state.current_index);
    } else if (state.current_index >= state.list_scroll + state.layout.visible_rows) {
        state.list_scroll = static_cast<uint32_t>(state.current_index - state.layout.visible_rows + 1);
    }
    clamp_scroll(state);
}

void load_current_document(ViewerState& state) {
    state.document =
        load_document(state.items[state.current_index], state.linear_display, state.dither_display);
    state.dirty = true;
}

void set_current_index(ViewerState& state, size_t index) {
    if (state.items.empty() || index >= state.items.size() || index == state.current_index) {
        return;
    }
    state.current_index = index;
    ensure_selection_visible(state);
    load_current_document(state);
}

#ifdef WK_VIEWER_GUI

bool pressed_once(const uint8_t* keys,
                  const std::array<uint8_t, static_cast<size_t>(MFB_KB_KEY_LAST) + 1>& previous,
                  mfb_key key) {
    if (keys == nullptr) {
        return false;
    }
    const size_t index = static_cast<size_t>(key);
    return index < previous.size() && keys[index] != 0 && previous[index] == 0;
}

bool mouse_pressed_once(const uint8_t* buttons,
                        const std::array<uint8_t, 8>& previous,
                        size_t index) {
    if (buttons == nullptr || index >= previous.size()) {
        return false;
    }
    return buttons[index] != 0 && previous[index] == 0;
}

void copy_key_buffer(std::array<uint8_t, static_cast<size_t>(MFB_KB_KEY_LAST) + 1>& destination,
                     const uint8_t* source) {
    if (source == nullptr) {
        destination.fill(0);
        return;
    }
    std::copy_n(source, destination.size(), destination.begin());
}

void copy_mouse_buttons(std::array<uint8_t, 8>& destination, const uint8_t* source) {
    if (source == nullptr) {
        destination.fill(0);
        return;
    }
    std::copy_n(source, destination.size(), destination.begin());
}

bool handle_keyboard(ViewerState& state, const uint8_t* keys) {
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_ESCAPE)) {
        return false;
    }

    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_RIGHT) ||
        pressed_once(keys, state.previous_keys, MFB_KB_KEY_D)) {
        set_current_index(state, (state.current_index + 1) % state.items.size());
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_LEFT) ||
        pressed_once(keys, state.previous_keys, MFB_KB_KEY_A)) {
        set_current_index(state, (state.current_index + state.items.size() - 1) % state.items.size());
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_DOWN)) {
        set_current_index(state, std::min(state.current_index + 1, state.items.size() - 1));
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_UP)) {
        set_current_index(state, state.current_index > 0 ? state.current_index - 1 : 0u);
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_HOME)) {
        set_current_index(state, 0u);
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_END)) {
        set_current_index(state, state.items.size() - 1);
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_I)) {
        state.show_details = !state.show_details;
        state.dirty = true;
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_L) && state.items.size() > 1) {
        state.show_list = !state.show_list;
        state.dirty = true;
    }
    if (pressed_once(keys, state.previous_keys, MFB_KB_KEY_TAB) && state.document.source_panel.has_value()) {
        state.compare_requested = !state.compare_requested;
        state.dirty = true;
    }

    return true;
}

void handle_mouse(ViewerState& state, mfb_window* window, const uint8_t* buttons) {
    const int mouse_x = mfb_get_mouse_x(window);
    const int mouse_y = mfb_get_mouse_y(window);
    const float scroll_y = mfb_get_mouse_scroll_y(window);

    if (state.layout.has_sidebar && state.layout.sidebar_list.contains(mouse_x, mouse_y)) {
        if (std::abs(scroll_y) > 0.01f) {
            const int delta = scroll_y > 0.0f ? -3 : 3;
            const int next_scroll = std::max(0, static_cast<int>(state.list_scroll) + delta);
            state.list_scroll = static_cast<uint32_t>(next_scroll);
            clamp_scroll(state);
            state.dirty = true;
        }

        if (mouse_pressed_once(buttons, state.previous_mouse_buttons, MFB_MOUSE_LEFT)) {
            const int relative_y = mouse_y - static_cast<int>(state.layout.sidebar_list.y);
            const size_t row = relative_y >= 0 ? static_cast<size_t>(relative_y / static_cast<int>(state.layout.sidebar_row_height)) : 0u;
            const size_t index = static_cast<size_t>(state.list_scroll) + row;
            if (index < state.items.size()) {
                set_current_index(state, index);
            }
        }
    }
}

#endif  // WK_VIEWER_GUI

// ---- Terminal (CLI) viewer ----
// Renders the same document the window shows, as ANSI truecolor
// half-blocks: one terminal cell shows two vertical pixels (the top one
// in the foreground, the bottom one in the background, U+2580). Used
// when no display is available (Termux, SSH, headless) or when --cli
// is given, so wkview never needs a window to show a picture.

struct TerminalSize {
    uint32_t cols = 80;
    uint32_t rows = 24;
};

TerminalSize terminal_size() {
    TerminalSize size;
    const char* columns = std::getenv("COLUMNS");
    if (columns != nullptr && columns[0] != '\0') {
        const long value = std::strtol(columns, nullptr, 10);
        if (value > 0) {
            size.cols = static_cast<uint32_t>(value);
        }
    }
#if defined(__unix__) || defined(__APPLE__)
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) {
            size.cols = ws.ws_col;
        }
        if (ws.ws_row > 0) {
            size.rows = ws.ws_row;
        }
    }
#elif defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) != 0) {
        const SHORT cols = info.srWindow.Right - info.srWindow.Left + 1;
        const SHORT rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (cols > 0) {
            size.cols = static_cast<uint32_t>(cols);
        }
        if (rows > 0) {
            size.rows = static_cast<uint32_t>(rows);
        }
    }
#endif
    return size;
}

struct Rgb8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

Rgb8 sample_average(const DisplayBuffer& image, double x0, double y0, double x1, double y1) {
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0)));
    const int ix1 = std::min(static_cast<int>(image.width), std::max(ix0 + 1, static_cast<int>(std::ceil(x1))));
    const int iy1 = std::min(static_cast<int>(image.height), std::max(iy0 + 1, static_cast<int>(std::ceil(y1))));
    uint64_t r = 0, g = 0, b = 0, count = 0;
    for (int y = iy0; y < iy1; ++y) {
        for (int x = ix0; x < ix1; ++x) {
            const uint32_t pixel = image.pixels[static_cast<size_t>(y) * image.width + x];
            const uint32_t alpha = channel_a(pixel);
            // Composite over black: a transparent pixel shows as black.
            r += static_cast<uint64_t>(channel_r(pixel)) * alpha / 255u;
            g += static_cast<uint64_t>(channel_g(pixel)) * alpha / 255u;
            b += static_cast<uint64_t>(channel_b(pixel)) * alpha / 255u;
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    return {static_cast<uint8_t>(r / count), static_cast<uint8_t>(g / count),
            static_cast<uint8_t>(b / count)};
}

// Rows (in terminal cells) an image needs at the given cell width.
// One cell shows two vertical pixels, so the row count is h/2 per w.
uint32_t terminal_rows_for(uint32_t src_width, uint32_t src_height, uint32_t cell_cols) {
    if (src_width == 0 || cell_cols == 0) {
        return 0;
    }
    return std::max(1u, static_cast<uint32_t>(std::lround(
                             static_cast<double>(src_height) * static_cast<double>(cell_cols) /
                             (2.0 * static_cast<double>(src_width)))));
}

std::string ansi_fg(const Rgb8& color) {
    return "\x1b[38;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" +
           std::to_string(color.b) + "m";
}

std::string ansi_bg(const Rgb8& color) {
    return "\x1b[48;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" +
           std::to_string(color.b) + "m";
}

uint8_t luma_of(const Rgb8& color) {
    return static_cast<uint8_t>(std::lround(0.299 * color.r + 0.587 * color.g + 0.114 * color.b));
}

// Render one image to lines of terminal text, exactly cell_cols wide
// and cell_rows tall. Color mode uses half-blocks; --no-color uses a
// brightness ramp so the picture stays readable on a dumb terminal.
std::vector<std::string> render_terminal_image(const DisplayBuffer& image, uint32_t cell_cols,
                                               uint32_t cell_rows, bool no_color) {
    static constexpr std::string_view kRamp = " .:-=+*#%@";
    std::vector<std::string> lines;
    if (image.width == 0 || image.height == 0 || cell_cols == 0 || cell_rows == 0) {
        return lines;
    }
    lines.reserve(cell_rows);

    const double step_x = static_cast<double>(image.width) / static_cast<double>(cell_cols);
    const double step_y = static_cast<double>(image.height) / static_cast<double>(cell_rows * 2);

    for (uint32_t row = 0; row < cell_rows; ++row) {
        std::string line;
        Rgb8 last_fg{0, 0, 0}, last_bg{0, 0, 0};
        bool have_color = false;
        for (uint32_t col = 0; col < cell_cols; ++col) {
            const double x0 = col * step_x;
            const double x1 = (col + 1) * step_x;
            const Rgb8 top = sample_average(image, x0, row * 2 * step_y, x1, (row * 2 + 1) * step_y);
            const Rgb8 bottom = sample_average(image, x0, (row * 2 + 1) * step_y, x1, (row * 2 + 2) * step_y);
            if (no_color) {
                const uint8_t luma = static_cast<uint8_t>((static_cast<unsigned>(luma_of(top)) +
                                                           static_cast<unsigned>(luma_of(bottom))) /
                                                          2u);
                const size_t index = static_cast<size_t>(luma) * (kRamp.size() - 1) / 255u;
                line.push_back(kRamp[index]);
                continue;
            }
            if (!have_color || top.r != last_fg.r || top.g != last_fg.g || top.b != last_fg.b ||
                bottom.r != last_bg.r || bottom.g != last_bg.g || bottom.b != last_bg.b) {
                line += ansi_fg(top);
                line += ansi_bg(bottom);
                last_fg = top;
                last_bg = bottom;
                have_color = true;
            }
            line += "\xE2\x96\x80";  // U+2580 UPPER HALF BLOCK
        }
        line += "\x1b[0m";
        lines.push_back(std::move(line));
    }
    return lines;
}

// Classic "hot" colormap: black -> red -> yellow -> white.
Rgb8 hot_color(double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    const auto channel = [](double x) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(std::lround(std::clamp(x, 0.0, 1.0) * 255.0), 0l, 255l));
    };
    return {channel(clamped * 3.0), channel(clamped * 3.0 - 1.0), channel(clamped * 3.0 - 2.0)};
}

// Per-pixel absolute difference (max over channels) between two images
// of the same size, rendered as a heatmap. Returns the lines plus the
// mean and max difference for the footer.
struct DiffRender {
    std::vector<std::string> lines;
    double mean = 0.0;
    uint8_t max = 0;
};

DiffRender render_terminal_diff(const DisplayBuffer& left, const DisplayBuffer& right,
                                uint32_t cell_cols, uint32_t cell_rows, bool no_color) {
    DiffRender result;
    if (left.width == 0 || left.height == 0 || left.width != right.width ||
        left.height != right.height || cell_cols == 0 || cell_rows == 0) {
        return result;
    }

    const double step_x = static_cast<double>(left.width) / static_cast<double>(cell_cols);
    const double step_y = static_cast<double>(left.height) / static_cast<double>(cell_rows * 2);
    uint64_t total = 0;
    uint64_t count = 0;

    // First pass: per-cell difference and the true max, so the heatmap
    // is scaled to the largest actual difference (a q75 encode differs
    // by ~5 at most, which would be invisible on an absolute 0-255
    // scale). The footer reports the absolute max, so the scale stays
    // interpretable.
    std::vector<uint8_t> cell_diffs(static_cast<size_t>(cell_cols) * cell_rows, 0);
    for (uint32_t row = 0; row < cell_rows; ++row) {
        for (uint32_t col = 0; col < cell_cols; ++col) {
            uint8_t cell_diff = 0;
            for (uint32_t sub = 0; sub < 2; ++sub) {
                const Rgb8 a = sample_average(left, col * step_x, (row * 2 + sub) * step_y,
                                              (col + 1) * step_x, (row * 2 + sub + 1) * step_y);
                const Rgb8 b = sample_average(right, col * step_x, (row * 2 + sub) * step_y,
                                              (col + 1) * step_x, (row * 2 + sub + 1) * step_y);
                const uint8_t d = std::max({static_cast<uint8_t>(std::abs(static_cast<int>(a.r) - static_cast<int>(b.r))),
                                            static_cast<uint8_t>(std::abs(static_cast<int>(a.g) - static_cast<int>(b.g))),
                                            static_cast<uint8_t>(std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)))});
                cell_diff = std::max(cell_diff, d);
            }
            cell_diffs[static_cast<size_t>(row) * cell_cols + col] = cell_diff;
            total += cell_diff;
            ++count;
            result.max = std::max(result.max, cell_diff);
        }
    }
    result.mean = count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count);

    for (uint32_t row = 0; row < cell_rows; ++row) {
        std::string line;
        Rgb8 last_color{0, 0, 0};
        bool have_color = false;
        for (uint32_t col = 0; col < cell_cols; ++col) {
            const uint8_t cell_diff = cell_diffs[static_cast<size_t>(row) * cell_cols + col];
            if (no_color) {
                static constexpr std::string_view kRamp = " .:-=+*#%@";
                const size_t index = result.max == 0
                                         ? 0
                                         : static_cast<size_t>(cell_diff) * (kRamp.size() - 1) /
                                               static_cast<size_t>(result.max);
                line.push_back(kRamp[index]);
                continue;
            }
            const Rgb8 color = hot_color(result.max == 0
                                             ? 0.0
                                             : static_cast<double>(cell_diff) /
                                                   static_cast<double>(result.max));
            if (!have_color || color.r != last_color.r || color.g != last_color.g ||
                color.b != last_color.b) {
                line += ansi_fg(color);
                line += ansi_bg(color);
                last_color = color;
                have_color = true;
            }
            line += "\xE2\x96\x88";  // U+2588 FULL BLOCK
        }
        line += "\x1b[0m";
        result.lines.push_back(std::move(line));
    }
    return result;
}

bool display_available() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    const char* display = std::getenv("DISPLAY");
    if (display != nullptr && display[0] != '\0') {
        return true;
    }
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if (wayland != nullptr && wayland[0] != '\0') {
        return true;
    }
    return false;
#endif
}

struct TerminalViewOptions {
    uint32_t width = 0;  // 0 = terminal width
    bool diff = false;
    bool no_color = false;
};

int run_terminal_viewer(const ViewerState& state, const TerminalViewOptions& options) {
    const ViewerDocument& document = state.document;
    const TerminalSize size = terminal_size();
    const uint32_t total_cols = options.width != 0 ? options.width : size.cols;
    const bool has_compare = state.compare_requested && document.source_panel.has_value();

    std::cout << document.decoded_panel.label << "  " << document.decoded_panel.image.width << 'x'
              << document.decoded_panel.image.height;
    if (has_compare) {
        std::cout << "  vs  " << document.source_panel->label << "  "
                  << document.source_panel->image.width << 'x' << document.source_panel->image.height;
    }
    std::cout << '\n';

    if (options.diff) {
        if (!has_compare) {
            std::cerr << "Error: --diff needs a source image to compare against\n";
            return 1;
        }
        if (document.decoded_panel.image.width != document.source_panel->image.width ||
            document.decoded_panel.image.height != document.source_panel->image.height) {
            std::cerr << "Error: --diff needs decoded and source at the same size\n";
            return 1;
        }
        const uint32_t cell_rows = terminal_rows_for(document.decoded_panel.image.width,
                                                     document.decoded_panel.image.height, total_cols);
        DiffRender diff = render_terminal_diff(document.decoded_panel.image,
                                               document.source_panel->image, total_cols, cell_rows,
                                               options.no_color);
        for (const std::string& line : diff.lines) {
            std::cout << line << '\n';
        }
        std::cout << "diff: mean " << std::fixed << std::setprecision(2) << diff.mean << "  max "
                  << static_cast<unsigned>(diff.max) << "  (black = identical, white = largest)\n";
        return 0;
    }

    if (!has_compare) {
        const uint32_t cell_rows = terminal_rows_for(document.decoded_panel.image.width,
                                                     document.decoded_panel.image.height, total_cols);
        for (const std::string& line :
             render_terminal_image(document.decoded_panel.image, total_cols, cell_rows,
                                   options.no_color)) {
            std::cout << line << '\n';
        }
        return 0;
    }

    // Side by side: each panel gets half the width minus the separator.
    const uint32_t panel_cols = total_cols >= 3 ? (total_cols - 3) / 2 : 1;
    const uint32_t rows = std::max(terminal_rows_for(document.decoded_panel.image.width,
                                                     document.decoded_panel.image.height, panel_cols),
                                   terminal_rows_for(document.source_panel->image.width,
                                                     document.source_panel->image.height, panel_cols));
    std::vector<std::string> left_lines = render_terminal_image(
        document.decoded_panel.image, panel_cols, rows, options.no_color);
    std::vector<std::string> right_lines = render_terminal_image(
        document.source_panel->image, panel_cols, rows, options.no_color);

    for (uint32_t row = 0; row < rows; ++row) {
        const std::string& left = row < left_lines.size() ? left_lines[row] : std::string();
        const std::string& right = row < right_lines.size() ? right_lines[row] : std::string();
        std::cout << left << "\x1b[0m \xE2\x94\x82 " << right << '\n';
    }
    return 0;
}

void print_usage() {
    std::cerr
        << "Usage: wkview <file.wk|directory> [source.{jpg,jpeg,png,ppm}|source_directory]\n"
        << "       wkview [options] <file.wk|directory> [source]\n"
        << "       wkview --help\n"
        << "\nModes (default is auto):\n"
        << "  --cli                 Render in the terminal instead of a window\n"
        << "  --window              Open a window (error when no display is available)\n"
        << "  auto: a window when a display exists, otherwise the terminal\n"
        << "\nTerminal options:\n"
        << "  --width N             Terminal output width in cells (default: terminal width)\n"
        << "  --diff                Heatmap of decoded-vs-source difference (needs a source)\n"
        << "  --no-color            Grayscale brightness ramp instead of truecolor\n"
        << "  --linear              Disable HDR tone-mapping (linear 16-bit downscale)\n"
        << "  --dither              Display dither on Flat/Sky blocks (Gaussian sigma=1, window only)\n"
        << "\nControls (window mode):\n"
        << "  Left/Right or A/D   Previous or next WK file\n"
        << "  Up/Down             Move selection in the file list\n"
        << "  Home/End            Jump to first or last file\n"
        << "  I                   Toggle details pane\n"
        << "  L                   Toggle file list sidebar\n"
        << "  Tab                 Toggle compare panel when source exists\n"
        << "  Mouse click         Select a file from the sidebar\n"
        << "  Mouse wheel         Scroll the file list\n"
        << "  Esc                 Exit viewer\n";
}

} // namespace

int main(int argc, char* argv[]) {
    enum class ViewMode { Auto, Cli, Window };
    ViewMode mode = ViewMode::Auto;
    TerminalViewOptions terminal_options;
    bool linear_display = false;
    bool dither_display = false;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--cli" || arg == "--terminal") {
            mode = ViewMode::Cli;
            continue;
        }
        if (arg == "--window" || arg == "--gui") {
            mode = ViewMode::Window;
            continue;
        }
        if (arg == "--linear") {
            linear_display = true;
            continue;
        }
        if (arg == "--dither") {
            dither_display = true;
            continue;
        }
        if (arg == "--diff") {
            terminal_options.diff = true;
            mode = ViewMode::Cli;
            continue;
        }
        if (arg == "--no-color") {
            terminal_options.no_color = true;
            continue;
        }
        if (arg == "--width") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --width needs a number\n";
                return 1;
            }
            const long value = std::strtol(argv[++i], nullptr, 10);
            if (value <= 0 || value > 10000) {
                std::cerr << "Error: --width must be between 1 and 10000\n";
                return 1;
            }
            terminal_options.width = static_cast<uint32_t>(value);
            continue;
        }
        if (arg.starts_with("--")) {
            std::cerr << "Error: unknown option: " << arg << '\n';
            print_usage();
            return 1;
        }
        positional.push_back(std::string(arg));
    }

    if (positional.empty()) {
        print_usage();
        return 1;
    }

    const std::string input_path = positional[0];
    const std::string source_hint = positional.size() >= 2 ? positional[1] : std::string();

    auto playlist = build_playlist(input_path, source_hint);
    if (!playlist) {
        std::cerr << "Error: " << playlist.error().message << '\n';
        return 1;
    }

    ViewerState state;
    state.items = std::move(playlist->first);
    state.current_index = playlist->second;
    state.show_list = state.items.size() > 1;
    state.compare_requested = state.items[state.current_index].has_source;
    state.linear_display = linear_display;
    state.dither_display = dither_display;
    state.document =
        load_document(state.items[state.current_index], state.linear_display, state.dither_display);

    // Smart default: a window when a display exists, otherwise the
    // terminal. --cli forces the terminal, --window forces a window.
    if (mode == ViewMode::Cli || (mode == ViewMode::Auto && !display_available())) {
        return run_terminal_viewer(state, terminal_options);
    }

#ifdef WK_VIEWER_GUI
    const uint32_t preferred_content_height = std::max(state.document.decoded_panel.image.height,
                                                       state.document.source_panel ? state.document.source_panel->image.height : 0u);
    const uint32_t preferred_info_height = std::max(info_block_height(state.document.decoded_panel),
                                                    state.document.source_panel ? info_block_height(*state.document.source_panel) : 0u);
    const uint32_t preferred_sidebar = state.show_list ? 260u : 0u;
    const uint32_t preferred_width = preferred_sidebar +
        (state.compare_requested && state.document.source_panel ? state.document.decoded_panel.image.width + state.document.source_panel->image.width + 48u
                                                               : state.document.decoded_panel.image.width + 24u);
    const uint32_t preferred_height = preferred_content_height + preferred_info_height + 98u;
    const unsigned initial_width = static_cast<unsigned>(std::clamp<uint32_t>(preferred_width, 960u, 1800u));
    const unsigned initial_height = static_cast<unsigned>(std::clamp<uint32_t>(preferred_height, 640u, 1100u));

    mfb_window* window = mfb_open_ex(build_window_title(state).c_str(),
                                     initial_width,
                                     initial_height,
                                     MFB_WF_RESIZABLE);
    if (window == nullptr) {
        // A display seemed to exist but the window still failed to open:
        // fall back to the terminal instead of giving up.
        std::cerr << "Note: failed to create viewer window, falling back to terminal\n";
        return run_terminal_viewer(state, terminal_options);
    }

    while (true) {
        const unsigned window_width = std::max(1u, mfb_get_window_width(window));
        const unsigned window_height = std::max(1u, mfb_get_window_height(window));
        if (window_width != state.last_window_width || window_height != state.last_window_height) {
            state.last_window_width = window_width;
            state.last_window_height = window_height;
            state.dirty = true;
        }

        if (state.dirty) {
            state.present = compose_window(state, state.last_window_width, state.last_window_height, state.layout);
            ensure_selection_visible(state);
            state.present = compose_window(state, state.last_window_width, state.last_window_height, state.layout);
            mfb_set_viewport(window, 0, 0, state.present.width, state.present.height);
            mfb_set_title(window, build_window_title(state).c_str());
            state.dirty = false;
        }

        const mfb_update_state update_state = mfb_update_ex(window,
                                                            const_cast<uint32_t*>(state.present.pixels.data()),
                                                            state.present.width,
                                                            state.present.height);
        if (update_state != MFB_STATE_OK) {
            break;
        }

        const uint8_t* keys = mfb_get_key_buffer(window);
        const uint8_t* mouse_buttons = mfb_get_mouse_button_buffer(window);
        if (!handle_keyboard(state, keys)) {
            break;
        }
        handle_mouse(state, window, mouse_buttons);

        copy_key_buffer(state.previous_keys, keys);
        copy_mouse_buttons(state.previous_mouse_buttons, mouse_buttons);

        if (!mfb_wait_sync(window)) {
            break;
        }
    }

    mfb_close(window);
    return 0;
#else
    std::cerr << "Error: this wkview was built without window support (WK_BUILD_VIEWER=OFF); use --cli\n";
    return 1;
#endif
}
