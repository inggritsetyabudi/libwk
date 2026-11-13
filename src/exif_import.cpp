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

#include "exif_import.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <string>

namespace wk::meta {

namespace {

constexpr uint16_t kTiffShort = 3;
constexpr uint16_t kTiffLong = 4;
constexpr uint16_t kTiffRational = 5;
constexpr uint16_t kTiffAscii = 2;
constexpr uint16_t kTiffByte = 1;

constexpr uint16_t kTagMake = 0x010F;
constexpr uint16_t kTagModel = 0x0110;
constexpr uint16_t kTagSoftware = 0x0131;
constexpr uint16_t kTagExifIfd = 0x8769;
constexpr uint16_t kTagGpsIfd = 0x8825;
constexpr uint16_t kTagIso = 0x8827;

constexpr uint16_t kGpsLatRef = 0x0001;
constexpr uint16_t kGpsLat = 0x0002;
constexpr uint16_t kGpsLonRef = 0x0003;
constexpr uint16_t kGpsLon = 0x0004;
constexpr uint16_t kGpsAltRef = 0x0005;
constexpr uint16_t kGpsAlt = 0x0006;
constexpr uint16_t kGpsTime = 0x0007;
constexpr uint16_t kGpsDate = 0x001D;

struct TiffEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t value_or_offset = 0;
    size_t entry_offset = 0;
};

class TiffReader {
public:
    explicit TiffReader(std::span<const uint8_t> data) : data_(data) {}

    std::expected<void, MetaError> initialize() {
        if (data_.size() < 8) {
            return std::unexpected(MetaError{MetaErrorCode::ParseError, "EXIF blob too short"});
        }

        if (data_[0] == 'I' && data_[1] == 'I') {
            big_endian_ = false;
        } else if (data_[0] == 'M' && data_[1] == 'M') {
            big_endian_ = true;
        } else {
            return std::unexpected(MetaError{MetaErrorCode::ParseError, "invalid TIFF byte order"});
        }

        auto magic = read_u16(2);
        if (!magic || *magic != 42) {
            return std::unexpected(MetaError{MetaErrorCode::ParseError, "invalid TIFF magic"});
        }
        return {};
    }

    std::expected<uint16_t, MetaError> read_u16(size_t offset) const {
        if (offset + 2 > data_.size()) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "unexpected end of TIFF data"});
        }
        if (big_endian_) {
            return static_cast<uint16_t>((data_[offset] << 8) | data_[offset + 1]);
        }
        return static_cast<uint16_t>(data_[offset] | (data_[offset + 1] << 8));
    }

    std::expected<uint32_t, MetaError> read_u32(size_t offset) const {
        if (offset + 4 > data_.size()) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "unexpected end of TIFF data"});
        }
        if (big_endian_) {
            return (static_cast<uint32_t>(data_[offset]) << 24) |
                   (static_cast<uint32_t>(data_[offset + 1]) << 16) |
                   (static_cast<uint32_t>(data_[offset + 2]) << 8) |
                   static_cast<uint32_t>(data_[offset + 3]);
        }
        return static_cast<uint32_t>(data_[offset]) |
               (static_cast<uint32_t>(data_[offset + 1]) << 8) |
               (static_cast<uint32_t>(data_[offset + 2]) << 16) |
               (static_cast<uint32_t>(data_[offset + 3]) << 24);
    }

    std::expected<std::vector<TiffEntry>, MetaError> read_ifd(uint32_t offset) const {
        auto count = read_u16(offset);
        if (!count) return std::unexpected(count.error());

        const size_t entries_start = static_cast<size_t>(offset) + 2;
        const size_t bytes_needed = entries_start + static_cast<size_t>(*count) * 12 + 4;
        if (bytes_needed > data_.size()) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "IFD extends beyond TIFF data"});
        }

        std::vector<TiffEntry> entries;
        entries.reserve(*count);
        for (uint16_t i = 0; i < *count; ++i) {
            const size_t entry_offset = entries_start + static_cast<size_t>(i) * 12;
            auto tag = read_u16(entry_offset + 0);
            auto type = read_u16(entry_offset + 2);
            auto item_count = read_u32(entry_offset + 4);
            auto value_or_offset = read_u32(entry_offset + 8);
            if (!tag || !type || !item_count || !value_or_offset) {
                return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "failed to parse IFD entry"});
            }
            entries.push_back(TiffEntry{*tag, *type, *item_count, *value_or_offset, entry_offset});
        }
        return entries;
    }

    size_t type_size(uint16_t type) const {
        switch (type) {
            case kTiffByte: return 1;
            case kTiffAscii: return 1;
            case kTiffShort: return 2;
            case kTiffLong: return 4;
            case kTiffRational: return 8;
            default: return 0;
        }
    }

    std::expected<std::span<const uint8_t>, MetaError> value_span(const TiffEntry& entry) const {
        const size_t bytes = static_cast<size_t>(entry.count) * type_size(entry.type);
        if (bytes == 0) {
            return std::unexpected(MetaError{MetaErrorCode::UnknownType, "unsupported TIFF field type"});
        }

        if (bytes <= 4) {
            if (entry.entry_offset + 8 + bytes > data_.size()) {
                return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "inline TIFF value exceeds input"});
            }
            return data_.subspan(entry.entry_offset + 8, bytes);
        }

        if (entry.value_or_offset + bytes > data_.size()) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "TIFF value offset exceeds input"});
        }
        return data_.subspan(entry.value_or_offset, bytes);
    }

    std::expected<uint32_t, MetaError> scalar_u32(const TiffEntry& entry) const {
        if (entry.count == 0) {
            return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "empty TIFF scalar"});
        }
        if (entry.type == kTiffShort) {
            return read_u16(entry.entry_offset + 8);
        }
        if (entry.type == kTiffLong) {
            return read_u32(entry.entry_offset + 8);
        }
        if (entry.type == kTiffByte) {
            auto span = value_span(entry);
            if (!span) return std::unexpected(span.error());
            return static_cast<uint32_t>((*span)[0]);
        }
        return std::unexpected(MetaError{MetaErrorCode::UnknownType, "unsupported TIFF scalar type"});
    }

    std::expected<std::string, MetaError> ascii_string(const TiffEntry& entry) const {
        auto span = value_span(entry);
        if (!span) return std::unexpected(span.error());
        std::string value(reinterpret_cast<const char*>(span->data()), span->size());
        while (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
        return value;
    }

    std::expected<Rational, MetaError> rational_at(const TiffEntry& entry, size_t index) const {
        if (entry.type != kTiffRational || index >= entry.count) {
            return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid TIFF rational request"});
        }
        const size_t value_offset = static_cast<size_t>(entry.value_or_offset) + index * 8;
        auto numerator = read_u32(value_offset);
        auto denominator = read_u32(value_offset + 4);
        if (!numerator || !denominator) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "invalid TIFF rational value"});
        }
        return Rational{static_cast<int32_t>(*numerator), static_cast<int32_t>(*denominator)};
    }

    [[nodiscard]] std::span<const uint8_t> data() const {
        return data_;
    }

private:
    std::span<const uint8_t> data_;
    bool big_endian_ = false;
};

const TiffEntry* find_entry(const std::vector<TiffEntry>& entries, uint16_t tag) {
    for (const auto& entry : entries) {
        if (entry.tag == tag) {
            return &entry;
        }
    }
    return nullptr;
}

double rational_to_double(const Rational& rational) {
    return rational.denominator != 0 ? static_cast<double>(rational.numerator) / rational.denominator : 0.0;
}

std::expected<double, MetaError> gps_coordinate(const TiffReader& reader,
                                                const TiffEntry* ref_entry,
                                                const TiffEntry* coord_entry,
                                                char positive_ref,
                                                char negative_ref) {
    if (!ref_entry || !coord_entry) {
        return std::unexpected(MetaError{MetaErrorCode::TagNotFound, "missing GPS coordinate tags"});
    }
    auto ref = reader.ascii_string(*ref_entry);
    if (!ref || ref->empty()) return std::unexpected(ref.error());
    if (coord_entry->type != kTiffRational || coord_entry->count < 3) {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "GPS coordinate is not rational[3]"});
    }

    auto degrees = reader.rational_at(*coord_entry, 0);
    auto minutes = reader.rational_at(*coord_entry, 1);
    auto seconds = reader.rational_at(*coord_entry, 2);
    if (!degrees || !minutes || !seconds) {
        return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "invalid GPS coordinate value"});
    }

    double value = rational_to_double(*degrees) + rational_to_double(*minutes) / 60.0 +
                   rational_to_double(*seconds) / 3600.0;
    const char ref_char = (*ref)[0];
    if (ref_char == negative_ref) {
        value = -value;
    } else if (ref_char != positive_ref) {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid GPS reference"});
    }
    return value;
}

std::expected<Timestamp, MetaError> gps_timestamp(const TiffReader& reader,
                                                  const TiffEntry* date_entry,
                                                  const TiffEntry* time_entry) {
    if (!date_entry || !time_entry) {
        return std::unexpected(MetaError{MetaErrorCode::TagNotFound, "missing GPS timestamp tags"});
    }
    auto date = reader.ascii_string(*date_entry);
    if (!date || date->size() != 10) {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid GPS date"});
    }
    if (time_entry->type != kTiffRational || time_entry->count < 3) {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid GPS time"});
    }

    auto hours = reader.rational_at(*time_entry, 0);
    auto minutes = reader.rational_at(*time_entry, 1);
    auto seconds = reader.rational_at(*time_entry, 2);
    if (!hours || !minutes || !seconds) {
        return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "invalid GPS time value"});
    }

    // The date must be exactly "YYYY:MM:DD": std::stoi on anything else
    // throws (std::invalid_argument / std::out_of_range) out of this
    // function with no handler, aborting the importer. Parse the digits
    // by hand, and reject months/days timegm would silently normalize
    // into a different, wrong date (e.g. month 13, day 45).
    auto parse_two_digits = [](const std::string& s, size_t pos) -> std::expected<int, MetaError> {
        const char hi = s[pos];
        const char lo = s[pos + 1];
        if (hi < '0' || hi > '9' || lo < '0' || lo > '9') {
            return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid GPS date"});
        }
        return (hi - '0') * 10 + (lo - '0');
    };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!is_digit((*date)[0]) || !is_digit((*date)[1]) || !is_digit((*date)[2]) ||
        !is_digit((*date)[3]) || (*date)[4] != ':' || (*date)[7] != ':') {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid GPS date"});
    }
    const int year = ((*date)[0] - '0') * 1000 + ((*date)[1] - '0') * 100 +
                     ((*date)[2] - '0') * 10 + ((*date)[3] - '0');
    auto month = parse_two_digits(*date, 5);
    if (!month) {
        return std::unexpected(month.error());
    }
    auto day = parse_two_digits(*date, 8);
    if (!day) {
        return std::unexpected(day.error());
    }
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31) {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "invalid GPS date"});
    }
    double seconds_value = rational_to_double(*seconds);
    int whole_seconds = static_cast<int>(seconds_value);
    int microseconds = static_cast<int>((seconds_value - whole_seconds) * 1000000.0 + 0.5);

    std::tm tm_value = {};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = *month - 1;
    tm_value.tm_mday = *day;
    tm_value.tm_hour = static_cast<int>(rational_to_double(*hours));
    tm_value.tm_min = static_cast<int>(rational_to_double(*minutes));
    tm_value.tm_sec = whole_seconds;

#ifdef _WIN32
    std::time_t timestamp = _mkgmtime(&tm_value);
#else
    std::time_t timestamp = timegm(&tm_value);
#endif
    if (timestamp < 0) {
        return std::unexpected(MetaError{MetaErrorCode::InvalidValue, "failed to convert GPS timestamp"});
    }
    return Timestamp{static_cast<uint64_t>(timestamp) * 1000000ULL + static_cast<uint64_t>(microseconds)};
}

}

std::expected<MetaBlock, MetaError> parse_exif_blob(std::span<const uint8_t> exif_blob) {
    TiffReader reader(exif_blob);
    auto init = reader.initialize();
    if (!init) return std::unexpected(init.error());

    auto ifd0_offset = reader.read_u32(4);
    if (!ifd0_offset) return std::unexpected(MetaError{MetaErrorCode::ParseError, "missing IFD0 offset"});

    auto ifd0 = reader.read_ifd(*ifd0_offset);
    if (!ifd0) return std::unexpected(ifd0.error());

    MetaBlock block;

    if (const auto* make = find_entry(*ifd0, kTagMake)) {
        if (auto value = reader.ascii_string(*make); value && !value->empty()) {
            block.set(Namespace::Capture, capture::MAKE, *value);
        }
    }
    if (const auto* model = find_entry(*ifd0, kTagModel)) {
        if (auto value = reader.ascii_string(*model); value && !value->empty()) {
            block.set(Namespace::Capture, capture::MODEL, *value);
        }
    }
    if (const auto* software = find_entry(*ifd0, kTagSoftware)) {
        if (auto value = reader.ascii_string(*software); value && !value->empty()) {
            block.set(Namespace::Capture, capture::SOFTWARE, *value);
        }
    }

    if (const auto* exif_ifd = find_entry(*ifd0, kTagExifIfd)) {
        auto exif_offset = reader.scalar_u32(*exif_ifd);
        if (exif_offset) {
            if (auto exif_entries = reader.read_ifd(*exif_offset); exif_entries) {
                if (const auto* iso = find_entry(*exif_entries, kTagIso)) {
                    if (auto value = reader.scalar_u32(*iso)) {
                        block.set(Namespace::Capture, capture::ISO, *value);
                    }
                }
            }
        }
    }

    if (const auto* gps_ifd = find_entry(*ifd0, kTagGpsIfd)) {
        auto gps_offset = reader.scalar_u32(*gps_ifd);
        if (gps_offset) {
            if (auto gps_entries = reader.read_ifd(*gps_offset); gps_entries) {
                auto lat = gps_coordinate(reader, find_entry(*gps_entries, kGpsLatRef),
                                          find_entry(*gps_entries, kGpsLat), 'N', 'S');
                if (lat) {
                    block.set(Namespace::Geo, geo::LAT, *lat);
                }

                auto lon = gps_coordinate(reader, find_entry(*gps_entries, kGpsLonRef),
                                          find_entry(*gps_entries, kGpsLon), 'E', 'W');
                if (lon) {
                    block.set(Namespace::Geo, geo::LON, *lon);
                }

                if (const auto* altitude = find_entry(*gps_entries, kGpsAlt)) {
                    if (auto alt = reader.rational_at(*altitude, 0)) {
                        float altitude_value = static_cast<float>(rational_to_double(*alt));
                        if (const auto* alt_ref = find_entry(*gps_entries, kGpsAltRef)) {
                            if (auto ref = reader.scalar_u32(*alt_ref); ref && *ref == 1) {
                                altitude_value = -altitude_value;
                            }
                        }
                        block.set(Namespace::Geo, geo::ALT, altitude_value);
                    }
                }

                auto gps_ts = gps_timestamp(reader, find_entry(*gps_entries, kGpsDate),
                                            find_entry(*gps_entries, kGpsTime));
                if (gps_ts) {
                    block.set(Namespace::Geo, geo::CAPTURE_TS, *gps_ts);
                }
            }
        }
    }

    return block;
}

std::expected<std::vector<uint8_t>, MetaError> extract_exif_blob_from_bytes(std::span<const uint8_t> file_bytes) {
    if (file_bytes.size() >= 8) {
        const bool is_tiff_le = file_bytes[0] == 'I' && file_bytes[1] == 'I' && file_bytes[2] == 42 && file_bytes[3] == 0;
        const bool is_tiff_be = file_bytes[0] == 'M' && file_bytes[1] == 'M' && file_bytes[2] == 0 && file_bytes[3] == 42;
        if (is_tiff_le || is_tiff_be) {
            return std::vector<uint8_t>(file_bytes.begin(), file_bytes.end());
        }
    }

    if (file_bytes.size() >= 6 && std::equal(file_bytes.begin(), file_bytes.begin() + 6,
                                              reinterpret_cast<const uint8_t*>("Exif\0\0"))) {
        return std::vector<uint8_t>(file_bytes.begin() + 6, file_bytes.end());
    }

    if (file_bytes.size() < 4 || file_bytes[0] != 0xFF || file_bytes[1] != 0xD8) {
        return std::unexpected(MetaError{MetaErrorCode::ParseError, "file does not contain EXIF data"});
    }

    size_t offset = 2;
    while (offset + 4 <= file_bytes.size()) {
        if (file_bytes[offset] != 0xFF) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "invalid JPEG marker"});
        }
        uint8_t marker = file_bytes[offset + 1];
        offset += 2;

        while (marker == 0xFF && offset < file_bytes.size()) {
            marker = file_bytes[offset++];
        }
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            continue;
        }
        if (offset + 2 > file_bytes.size()) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "truncated JPEG segment"});
        }

        uint16_t segment_size = static_cast<uint16_t>((file_bytes[offset] << 8) | file_bytes[offset + 1]);
        if (segment_size < 2 || offset + segment_size > file_bytes.size()) {
            return std::unexpected(MetaError{MetaErrorCode::MalformedInput, "invalid JPEG segment size"});
        }

        if (marker == 0xE1 && segment_size >= 8) {
            const size_t payload_offset = offset + 2;
            const size_t payload_size = segment_size - 2;
            if (payload_size >= 6 && std::equal(file_bytes.begin() + payload_offset,
                                                file_bytes.begin() + payload_offset + 6,
                                                reinterpret_cast<const uint8_t*>("Exif\0\0"))) {
                return std::vector<uint8_t>(file_bytes.begin() + payload_offset + 6,
                                            file_bytes.begin() + payload_offset + payload_size);
            }
        }

        offset += segment_size;
    }

    return std::unexpected(MetaError{MetaErrorCode::TagNotFound, "no EXIF APP1 segment found"});
}

}
