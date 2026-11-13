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
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::expected<wk::meta::Namespace, std::string> parse_namespace(const std::string& ns_str) {
    if (ns_str == "CAPTURE")  return wk::meta::Namespace::Capture;
    if (ns_str == "GEO")      return wk::meta::Namespace::Geo;
    if (ns_str == "TIME")     return wk::meta::Namespace::Time;
    if (ns_str == "RIGHTS")   return wk::meta::Namespace::Rights;
    if (ns_str == "CONTENT")  return wk::meta::Namespace::Content;
    if (ns_str == "ANIM")     return wk::meta::Namespace::Anim;
    if (ns_str == "REGION")   return wk::meta::Namespace::Region;
    if (ns_str == "DEVICE")   return wk::meta::Namespace::Device;
    if (ns_str == "RATING")   return wk::meta::Namespace::Rating;
    if (ns_str == "CUSTOM")   return wk::meta::Namespace::Custom;
    if (ns_str == "PROV_REF") return wk::meta::Namespace::ProvRef;
    // An unknown namespace used to silently become Custom, writing the
    // entry under the wrong namespace. Fail loudly instead.
    return std::unexpected("unknown namespace '" + ns_str +
                           "' (expected one of CAPTURE, GEO, TIME, RIGHTS, CONTENT, "
                           "ANIM, REGION, DEVICE, RATING, CUSTOM, PROV_REF)");
}

namespace {
struct TagName {
    const char* ns;
    const char* name;
    uint16_t tag;
};
// Every documented tag name (see include/wk/wkmeta.hpp), so a real tag
// is never mis-resolved to tag 0 the way atoi() used to do.
constexpr TagName kTagNames[] = {
    {"GEO", "LAT", wk::meta::geo::LAT},
    {"GEO", "LON", wk::meta::geo::LON},
    {"GEO", "ALT", wk::meta::geo::ALT},
    {"GEO", "HPOS_ERR", wk::meta::geo::HPOS_ERR},
    {"GEO", "VPOS_ERR", wk::meta::geo::VPOS_ERR},
    {"GEO", "SPEED", wk::meta::geo::SPEED},
    {"GEO", "HEADING", wk::meta::geo::HEADING},
    {"GEO", "PITCH", wk::meta::geo::PITCH},
    {"GEO", "ROLL", wk::meta::geo::ROLL},
    {"GEO", "COUNTRY", wk::meta::geo::COUNTRY},
    {"GEO", "REGION", wk::meta::geo::REGION},
    {"GEO", "CITY", wk::meta::geo::CITY},
    {"GEO", "SUBLOC", wk::meta::geo::SUBLOC},
    {"GEO", "PLACE_NAME", wk::meta::geo::PLACE_NAME},
    {"GEO", "WHAT3WORDS", wk::meta::geo::WHAT3WORDS},
    {"GEO", "PLUS_CODE", wk::meta::geo::PLUS_CODE},
    {"GEO", "CAPTURE_TS", wk::meta::geo::CAPTURE_TS},
    {"GEO", "DEST_LAT", wk::meta::geo::DEST_LAT},
    {"GEO", "DEST_LON", wk::meta::geo::DEST_LON},
    {"TIME", "CAPTURE_UTC", wk::meta::time_tags::CAPTURE_UTC},
    {"TIME", "OFFSET_SEC", wk::meta::time_tags::OFFSET_SEC},
    {"TIME", "CREATED_UTC", wk::meta::time_tags::CREATED_UTC},
    {"TIME", "MODIFIED_UTC", wk::meta::time_tags::MODIFIED_UTC},
    {"TIME", "DIGITIZED_UTC", wk::meta::time_tags::DIGITIZED_UTC},
    {"TIME", "SUBSEC", wk::meta::time_tags::SUBSEC},
    {"TIME", "TIMEZONE_ID", wk::meta::time_tags::TIMEZONE_ID},
    {"CAPTURE", "MAKE", wk::meta::capture::MAKE},
    {"CAPTURE", "MODEL", wk::meta::capture::MODEL},
    {"CAPTURE", "SERIAL", wk::meta::capture::SERIAL},
    {"CAPTURE", "LENS_MAKE", wk::meta::capture::LENS_MAKE},
    {"CAPTURE", "LENS_MODEL", wk::meta::capture::LENS_MODEL},
    {"CAPTURE", "FOCAL_LEN", wk::meta::capture::FOCAL_LEN},
    {"CAPTURE", "FOCAL_35MM", wk::meta::capture::FOCAL_35MM},
    {"CAPTURE", "FNUMBER", wk::meta::capture::FNUMBER},
    {"CAPTURE", "EXPOSURE_TIME", wk::meta::capture::EXPOSURE_TIME},
    {"CAPTURE", "ISO", wk::meta::capture::ISO},
    {"CAPTURE", "EV_COMP", wk::meta::capture::EV_COMP},
    {"CAPTURE", "FLASH", wk::meta::capture::FLASH},
    {"CAPTURE", "WHITE_BAL", wk::meta::capture::WHITE_BAL},
    {"CAPTURE", "METERING", wk::meta::capture::METERING},
    {"CAPTURE", "SCENE_TYPE", wk::meta::capture::SCENE_TYPE},
    {"CAPTURE", "SHARPNESS", wk::meta::capture::SHARPNESS},
    {"CAPTURE", "SATURATION", wk::meta::capture::SATURATION},
    {"CAPTURE", "SOFTWARE", wk::meta::capture::SOFTWARE},
    {"CAPTURE", "UNIQUE_IMG_ID", wk::meta::capture::UNIQUE_IMG_ID},
    {"CAPTURE", "BURST_INDEX", wk::meta::capture::BURST_INDEX},
    {"CAPTURE", "BURST_COUNT", wk::meta::capture::BURST_COUNT},
    {"CAPTURE", "HDR_FRAMES", wk::meta::capture::HDR_FRAMES},
    {"RIGHTS", "CREATOR", wk::meta::rights::CREATOR},
    {"RIGHTS", "CREATOR_ROLE", wk::meta::rights::CREATOR_ROLE},
    {"RIGHTS", "COPYRIGHT", wk::meta::rights::COPYRIGHT},
    {"RIGHTS", "LICENSE_SPDX", wk::meta::rights::LICENSE_SPDX},
    {"RIGHTS", "LICENSE", wk::meta::rights::LICENSE_SPDX},
    {"RIGHTS", "LICENSE_URL", wk::meta::rights::LICENSE_URL},
    {"RIGHTS", "CREDIT_LINE", wk::meta::rights::CREDIT_LINE},
    {"RIGHTS", "SOURCE", wk::meta::rights::SOURCE},
    {"RIGHTS", "INSTRUCTIONS", wk::meta::rights::INSTRUCTIONS},
    {"RIGHTS", "EMBARGO_UTC", wk::meta::rights::EMBARGO_UTC},
    {"CONTENT", "TITLE", wk::meta::content::TITLE},
    {"CONTENT", "DESCRIPTION", wk::meta::content::DESCRIPTION},
    {"CONTENT", "ALT_TEXT", wk::meta::content::ALT_TEXT},
    {"CONTENT", "KEYWORDS", wk::meta::content::KEYWORDS},
    {"CONTENT", "GENRE", wk::meta::content::GENRE},
    {"CONTENT", "SUBJECT_CODES", wk::meta::content::SUBJECT_CODES},
    {"CONTENT", "SCENE_CODES", wk::meta::content::SCENE_CODES},
    {"CONTENT", "EVENT", wk::meta::content::EVENT},
    {"CONTENT", "OBJECT_NAME", wk::meta::content::OBJECT_NAME},
    {"CONTENT", "CATEGORY", wk::meta::content::CATEGORY},
    {"CONTENT", "LANGUAGE", wk::meta::content::LANGUAGE},
    {"CONTENT", "SMARTCROP", wk::meta::content::SMARTCROP},
    {"ANIM", "TITLE", wk::meta::anim_tags::TITLE},
    {"ANIM", "DESCRIPTION", wk::meta::anim_tags::DESCRIPTION},
    {"ANIM", "CREATOR", wk::meta::anim_tags::CREATOR},
    {"ANIM", "DURATION_MS", wk::meta::anim_tags::DURATION_MS},
    {"ANIM", "FPS_TARGET", wk::meta::anim_tags::FPS_TARGET},
    {"ANIM", "CATEGORY", wk::meta::anim_tags::CATEGORY},
    {"ANIM", "SCENE_LABELS", wk::meta::anim_tags::SCENE_LABELS},
    {"ANIM", "THUMB_TILE", wk::meta::anim_tags::THUMB_TILE},
    {"REGION", "NAME", wk::meta::region::NAME},
    {"REGION", "TYPE", wk::meta::region::TYPE},
    {"REGION", "ROLE", wk::meta::region::ROLE},
    {"REGION", "RECT_NORM", wk::meta::region::RECT_NORM},
    {"REGION", "CONFIDENCE", wk::meta::region::CONFIDENCE},
    {"REGION", "PERSON_NAME", wk::meta::region::PERSON_NAME},
    {"REGION", "PERSON_ID", wk::meta::region::PERSON_ID},
    {"REGION", "OBJECT_CLASS", wk::meta::region::OBJECT_CLASS},
    {"REGION", "CREATOR", wk::meta::region::CREATOR},
    {"RATING", "STARS", wk::meta::rating::STARS},
    {"RATING", "AUDIENCE", wk::meta::rating::AUDIENCE},
    {"RATING", "NSFW", wk::meta::rating::NSFW},
    {"RATING", "AI_GENERATED", wk::meta::rating::AI_GENERATED},
    {"RATING", "QUALITY_SCORE", wk::meta::rating::QUALITY_SCORE},
    {"RATING", "CAPTION_SRC", wk::meta::rating::CAPTION_SRC},
};
}  // namespace

std::expected<uint16_t, std::string> parse_tag(const std::string& ns_str, const std::string& tag_str) {
    using namespace wk::meta;

    for (const auto& known : kTagNames) {
        if (ns_str == known.ns && tag_str == known.name) {
            return known.tag;
        }
    }

    // Numeric tag: 0xNNNN hex or plain decimal, strictly parsed and
    // range-checked. atoi()/strtoul() used to turn any garbage into 0
    // (and wrap out-of-range numbers), silently writing the wrong tag.
    if (tag_str.size() > 2 && tag_str[0] == '0' && (tag_str[1] == 'x' || tag_str[1] == 'X')) {
        uint32_t value = 0;
        auto r = std::from_chars(tag_str.data() + 2, tag_str.data() + tag_str.size(), value, 16);
        if (r.ec == std::errc() && r.ptr == tag_str.data() + tag_str.size() && value <= 0xFFFF) {
            return static_cast<uint16_t>(value);
        }
        return std::unexpected("invalid hex tag '" + tag_str + "' (expected 0x0000-0xFFFF)");
    }
    if (!tag_str.empty() &&
        std::all_of(tag_str.begin(), tag_str.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        uint32_t value = 0;
        auto r = std::from_chars(tag_str.data(), tag_str.data() + tag_str.size(), value, 10);
        if (r.ec == std::errc() && r.ptr == tag_str.data() + tag_str.size() && value <= 0xFFFF) {
            return static_cast<uint16_t>(value);
        }
        return std::unexpected("invalid decimal tag '" + tag_str + "' (expected 0-65535)");
    }
    return std::unexpected("unknown tag '" + tag_str + "' for namespace '" + ns_str +
                           "' (use a documented tag name or a 0xNNNN number)");
}

bool parse_key(const std::string& key, std::string& ns_out, std::string& tag_out) {
    const auto dot = key.find('.');
    if (dot == std::string::npos) {
        return false;
    }
    ns_out = key.substr(0, dot);
    tag_out = key.substr(dot + 1);
    return true;
}

// Strict number parsing: atof()/atoi() used to turn any garbage into 0
// (and silently truncate out-of-range values), so "--set GEO.LAT abc"
// wrote 0.0 as if the user had asked for it.
std::expected<double, std::string> parse_double_strict(const std::string& s, const char* what) {
    double value = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
        return std::unexpected(std::string("invalid number '") + s + "' for " + what);
    }
    return value;
}

std::expected<uint32_t, std::string> parse_u32_strict(const std::string& s, const char* what) {
    uint32_t value = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
        return std::unexpected(std::string("invalid number '") + s + "' for " + what);
    }
    return value;
}

std::expected<int32_t, std::string> parse_i32_strict(const std::string& s, const char* what) {
    int32_t value = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
        return std::unexpected(std::string("invalid number '") + s + "' for " + what);
    }
    return value;
}

std::expected<wk::meta::Value, std::string> parse_value_str(const std::string& ns_str,
                                                            const std::string& tag_str,
                                                            const std::string& value_str) {
    using namespace wk::meta;

    auto ranged_double = [&](double lo, double hi, const char* what)
        -> std::expected<double, std::string> {
        auto v = parse_double_strict(value_str, what);
        if (!v)
            return v;
        if (*v < lo || *v > hi) {
            return std::unexpected(std::string("value '") + value_str + "' for " + what +
                                   " is out of range [" + std::to_string(lo) + ", " +
                                   std::to_string(hi) + "]");
        }
        return v;
    };

    if (ns_str == "GEO" && tag_str == "LAT") {
        auto v = ranged_double(-90.0, 90.0, "GEO.LAT");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<double>(*v);
    }
    if (ns_str == "GEO" && tag_str == "LON") {
        auto v = ranged_double(-180.0, 180.0, "GEO.LON");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<double>(*v);
    }
    if (ns_str == "GEO" && tag_str == "DEST_LAT") {
        auto v = ranged_double(-90.0, 90.0, "GEO.DEST_LAT");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<double>(*v);
    }
    if (ns_str == "GEO" && tag_str == "DEST_LON") {
        auto v = ranged_double(-180.0, 180.0, "GEO.DEST_LON");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<double>(*v);
    }
    if (ns_str == "GEO" && tag_str == "ALT") {
        auto v = parse_double_strict(value_str, "GEO.ALT");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if (ns_str == "GEO" && (tag_str == "HPOS_ERR" || tag_str == "VPOS_ERR" || tag_str == "SPEED")) {
        auto v = parse_double_strict(value_str, ("GEO." + tag_str).c_str());
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if (ns_str == "GEO" && tag_str == "HEADING") {
        auto v = ranged_double(0.0, 360.0, "GEO.HEADING");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if (ns_str == "GEO" && tag_str == "PITCH") {
        auto v = ranged_double(-90.0, 90.0, "GEO.PITCH");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if (ns_str == "GEO" && tag_str == "ROLL") {
        auto v = ranged_double(-180.0, 180.0, "GEO.ROLL");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if (ns_str == "RATING" &&
        (tag_str == "STARS" || tag_str == "AUDIENCE" || tag_str == "NSFW" || tag_str == "AI_GENERATED")) {
        auto v = parse_u32_strict(value_str, ("RATING." + tag_str).c_str());
        if (!v)
            return std::unexpected(v.error());
        if (*v > 255) {
            return std::unexpected("value '" + value_str + "' for RATING." + tag_str +
                                   " is out of range [0, 255]");
        }
        return static_cast<uint8_t>(*v);
    }
    if (ns_str == "RATING" && tag_str == "QUALITY_SCORE") {
        auto v = parse_double_strict(value_str, "RATING.QUALITY_SCORE");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if (ns_str == "CAPTURE" && tag_str == "ISO") {
        auto v = parse_u32_strict(value_str, "CAPTURE.ISO");
        if (!v)
            return std::unexpected(v.error());
        return *v;
    }
    if (ns_str == "CAPTURE" &&
        (tag_str == "FOCAL_LEN" || tag_str == "FNUMBER" || tag_str == "EXPOSURE_TIME" || tag_str == "EV_COMP")) {
        auto slash = value_str.find('/');
        if (slash == std::string::npos) {
            return std::unexpected("invalid rational '" + value_str + "' for CAPTURE." + tag_str +
                                   " (expected NUM/DEN)");
        }
        auto num = parse_i32_strict(value_str.substr(0, slash), ("CAPTURE." + tag_str).c_str());
        if (!num)
            return std::unexpected(num.error());
        auto den = parse_i32_strict(value_str.substr(slash + 1), ("CAPTURE." + tag_str).c_str());
        if (!den)
            return std::unexpected(den.error());
        if (*den == 0) {
            return std::unexpected("rational denominator is 0 for CAPTURE." + tag_str);
        }
        return Rational{*num, *den};
    }
    if (ns_str == "ANIM" && tag_str == "DURATION_MS") {
        auto v = parse_u32_strict(value_str, "ANIM.DURATION_MS");
        if (!v)
            return std::unexpected(v.error());
        return *v;
    }
    if (ns_str == "ANIM" && tag_str == "FPS_TARGET") {
        auto v = parse_double_strict(value_str, "ANIM.FPS_TARGET");
        if (!v)
            return std::unexpected(v.error());
        return static_cast<float>(*v);
    }
    if ((ns_str == "TIME" &&
         (tag_str == "CAPTURE_UTC" || tag_str == "CREATED_UTC" || tag_str == "MODIFIED_UTC" ||
          tag_str == "DIGITIZED_UTC")) ||
        (ns_str == "GEO" && tag_str == "CAPTURE_TS") ||
        (ns_str == "RIGHTS" && tag_str == "EMBARGO_UTC")) {
        // ISO-8601 UTC timestamp, e.g. 2024-04-09T12:34:56Z.
        bool shape_ok = value_str.size() >= 20 && value_str[4] == '-' && value_str[7] == '-' &&
                        value_str[10] == 'T' && value_str[13] == ':' && value_str[16] == ':';
        if (!shape_ok) {
            return std::unexpected("invalid timestamp '" + value_str + "' for " + ns_str + "." +
                                   tag_str + " (expected ISO-8601, e.g. 2024-04-09T12:34:56Z)");
        }
        return Timestamp::from_iso8601(value_str);
    }

    const auto colon = value_str.find(':');
    if (colon != std::string::npos && colon < 5 &&
        (ns_str == "CONTENT" || ns_str == "RIGHTS" || ns_str == "ANIM" || ns_str == "REGION")) {
        return wk::meta::LocalizedString{value_str.substr(0, colon), value_str.substr(colon + 1)};
    }

    return value_str;
}

wk::Result<std::vector<uint8_t>> read_file_bytes(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to open file"});
    }

    const std::streamsize size = file.tellg();
    if (size < 0) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to determine file size"});
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(wk::Error{wk::ErrorCode::IoError, "failed to read file"});
    }
    return bytes;
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

void print_usage() {
    std::cerr
        << "Usage: wkmeta-edit [options] <input.wk>\n\n"
        << "Options:\n"
        << "  --set NS.TAG VALUE   Set metadata field (example: --set GEO.LAT 1.234)\n"
        << "  --delete NS.TAG      Delete metadata field\n"
        << "  --import-exif FILE   Import EXIF from JPEG/TIFF donor file\n"
        << "  --export-json        Print WKMETA as JSON to stdout\n"
        << "  --version            Print version\n"
        << "  -o, --output FILE    Output file (default: in-place edit)\n"
        << "  -h, --help           Show this help\n";
}

}

int main(int argc, char* argv[]) {
    struct SetOp {
        std::string key;
        std::string value;
    };

    std::vector<SetOp> set_ops;
    std::vector<std::string> delete_ops;
    std::string input_path;
    std::string output_path;
    std::string exif_source_path;
    bool export_json = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--set") {
            if (i + 2 >= argc) {
                std::cerr << "Error: --set requires NS.TAG and VALUE\n";
                return 1;
            }
            set_ops.push_back({argv[++i], argv[++i]});
        } else if (arg == "--delete") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --delete requires NS.TAG\n";
                return 1;
            }
            delete_ops.push_back(argv[++i]);
        } else if (arg == "--import-exif") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --import-exif requires a donor file\n";
                return 1;
            }
            exif_source_path = argv[++i];
        } else if (arg == "--export-json") {
            export_json = true;
        } else if (arg == "--version") {
            std::cout << wk::version() << '\n';
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --output requires a file path\n";
                return 1;
            }
            output_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option " << arg << '\n';
            print_usage();
            return 1;
        } else if (input_path.empty()) {
            input_path = arg;
        } else {
            std::cerr << "Error: unexpected extra argument " << arg << '\n';
            return 1;
        }
    }

    if (input_path.empty()) {
        print_usage();
        return 1;
    }

    auto input_bytes = read_file_bytes(input_path);
    if (!input_bytes) {
        std::cerr << "Error: " << input_bytes.error().message << '\n';
        return 1;
    }

    auto file = wk::parse_container(*input_bytes);
    if (!file) {
        std::cerr << "Error: " << file.error().message << '\n';
        return 1;
    }

    if (!file->metadata) {
        file->metadata = wk::meta::MetaBlock{};
    }

    for (const auto& operation : set_ops) {
        std::string ns_str;
        std::string tag_str;
        if (!parse_key(operation.key, ns_str, tag_str)) {
            std::cerr << "Error: invalid metadata key " << operation.key << " (expected NS.TAG)\n";
            return 1;
        }

        auto ns = parse_namespace(ns_str);
        if (!ns) {
            std::cerr << "Error: " << ns.error() << '\n';
            return 1;
        }
        auto tag = parse_tag(ns_str, tag_str);
        if (!tag) {
            std::cerr << "Error: " << tag.error() << '\n';
            return 1;
        }
        auto value = parse_value_str(ns_str, tag_str, operation.value);
        if (!value) {
            std::cerr << "Error: " << value.error() << '\n';
            return 1;
        }

        auto set_result = file->metadata->set(*ns, *tag, std::move(*value));
        if (!set_result) {
            std::cerr << "Error: failed to set " << operation.key << ": " << set_result.error().message << '\n';
            return 1;
        }
    }

    for (const auto& key : delete_ops) {
        std::string ns_str;
        std::string tag_str;
        if (!parse_key(key, ns_str, tag_str)) {
            std::cerr << "Error: invalid metadata key " << key << " (expected NS.TAG)\n";
            return 1;
        }
        auto ns = parse_namespace(ns_str);
        if (!ns) {
            std::cerr << "Error: " << ns.error() << '\n';
            return 1;
        }
        auto tag = parse_tag(ns_str, tag_str);
        if (!tag) {
            std::cerr << "Error: " << tag.error() << '\n';
            return 1;
        }
        file->metadata->remove(*ns, *tag);
    }

    if (!exif_source_path.empty()) {
        auto donor_bytes = read_file_bytes(exif_source_path);
        if (!donor_bytes) {
            std::cerr << "Error: " << donor_bytes.error().message << '\n';
            return 1;
        }

        auto exif_blob = wk::meta::extract_exif_blob_from_bytes(*donor_bytes);
        if (!exif_blob) {
            std::cerr << "Error: EXIF import failed: " << exif_blob.error().message << '\n';
            return 1;
        }

        auto imported = wk::meta::MetaBlock::from_exif(*exif_blob);
        if (!imported) {
            std::cerr << "Error: EXIF parse failed: " << imported.error().message << '\n';
            return 1;
        }

        merge_metadata(*file->metadata, *imported);
    }

    if (export_json) {
        std::cout << file->metadata->to_json();
        if (set_ops.empty() && delete_ops.empty() && exif_source_path.empty()) {
            return 0;
        }
    }

    const bool metadata_empty = !file->metadata || file->metadata->entries.empty();
    if (metadata_empty) {
        file->metadata.reset();
        file->header.flags = static_cast<uint16_t>(file->header.flags & static_cast<uint16_t>(~wk::FHDR_FLAG_HAS_WKMETA));
    } else {
        file->header.flags |= wk::FHDR_FLAG_HAS_WKMETA;
    }

    auto output_bytes = wk::write_container(*file);
    if (!output_bytes) {
        std::cerr << "Error: " << output_bytes.error().message << '\n';
        return 1;
    }

    if (output_path.empty()) {
        output_path = input_path;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        std::cerr << "Error: failed to open output file\n";
        return 1;
    }
    output.write(reinterpret_cast<const char*>(output_bytes->data()), static_cast<std::streamsize>(output_bytes->size()));
    if (!output) {
        std::cerr << "Error: failed to write output file\n";
        return 1;
    }

    std::cerr << "Written " << output_path << " (" << output_bytes->size() << " bytes)\n";
    return 0;
}
