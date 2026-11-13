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

#include <wk/wkmeta.hpp>
#include "common.h"
#include "exif_import.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <charconv>

namespace wk::meta
{

    std::string Timestamp::to_iso8601() const
    {
        time_t secs = static_cast<time_t>(microseconds / 1000000ULL);
        uint32_t usec = static_cast<uint32_t>(microseconds % 1000000ULL);

        struct tm utc_tm;
#ifdef _WIN32
        gmtime_s(&utc_tm, &secs);
#else
        gmtime_r(&secs, &utc_tm);
#endif

        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc_tm);

        if (usec > 0)
        {
            char usec_buf[16];
            std::snprintf(usec_buf, sizeof(usec_buf), ".%06u", usec);
            std::strncat(buf, usec_buf, sizeof(buf) - std::strlen(buf) - 1);
        }
        std::strncat(buf, "Z", sizeof(buf) - std::strlen(buf) - 1);
        return buf;
    }

    Timestamp Timestamp::from_iso8601(std::string_view s)
    {
        Timestamp ts;

        if (s.size() < 19)
            return ts;

        struct tm tm_val = {};
        int year, month, day, hour, minute, second;
        if (std::sscanf(s.data(), "%d-%d-%dT%d:%d:%d",
                        &year, &month, &day, &hour, &minute, &second) < 6)
        {
            return ts;
        }

        tm_val.tm_year = year - 1900;
        tm_val.tm_mon = month - 1;
        tm_val.tm_mday = day;
        tm_val.tm_hour = hour;
        tm_val.tm_min = minute;
        tm_val.tm_sec = second;

#ifdef _WIN32
        time_t t = _mkgmtime(&tm_val);
#else
        time_t t = timegm(&tm_val);
#endif

        ts.microseconds = static_cast<uint64_t>(t) * 1000000ULL;

        auto dot_pos = s.find('.');
        if (dot_pos != std::string_view::npos)
        {
            auto end_pos = s.find('Z', dot_pos);
            if (end_pos == std::string_view::npos)
                end_pos = s.size();
            std::string frac_str(s.substr(dot_pos + 1, end_pos - dot_pos - 1));
            while (frac_str.size() < 6)
                frac_str += '0';
            frac_str = frac_str.substr(0, 6);
            uint32_t usec = 0;
            std::from_chars(frac_str.data(), frac_str.data() + frac_str.size(), usec);
            ts.microseconds += usec;
        }

        return ts;
    }

    std::string Uuid::to_string() const
    {
        char buf[37];
        std::snprintf(buf, sizeof(buf),
                      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      bytes[0], bytes[1], bytes[2], bytes[3],
                      bytes[4], bytes[5], bytes[6], bytes[7],
                      bytes[8], bytes[9], bytes[10], bytes[11],
                      bytes[12], bytes[13], bytes[14], bytes[15]);
        return buf;
    }

    Uuid Uuid::from_string(std::string_view s)
    {
        Uuid uuid;
        if (s.size() < 36)
            return uuid;

        int idx = 0;
        for (size_t i = 0; i < s.size() && idx < 16; i++)
        {
            if (s[i] == '-')
                continue;
            if (i + 1 >= s.size())
                break;

            char hex[3] = {s[i], s[i + 1], '\0'};
            uuid.bytes[idx++] = static_cast<uint8_t>(std::strtoul(hex, nullptr, 16));
            i++;
        }
        return uuid;
    }

    Uuid Uuid::generate()
    {

        Uuid uuid;
        static uint64_t counter = 0;
        uint64_t seed = static_cast<uint64_t>(std::time(nullptr)) ^ (++counter);

        for (int i = 0; i < 16; i++)
        {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            uuid.bytes[i] = static_cast<uint8_t>(seed >> 32);
        }

        uuid.bytes[6] = (uuid.bytes[6] & 0x0F) | 0x40;

        uuid.bytes[8] = (uuid.bytes[8] & 0x3F) | 0x80;
        return uuid;
    }

    bool Struct::operator==(const Struct &other) const
    {
        return fields == other.fields;
    }

    static TypeId type_id_from_value(const Value &v)
    {
        return std::visit([](const auto &val) -> TypeId
                          {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, uint8_t>) return TypeId::UINT8;
        else if constexpr (std::is_same_v<T, uint16_t>) return TypeId::UINT16;
        else if constexpr (std::is_same_v<T, uint32_t>) return TypeId::UINT32;
        else if constexpr (std::is_same_v<T, uint64_t>) return TypeId::UINT64;
        else if constexpr (std::is_same_v<T, int32_t>) return TypeId::INT32;
        else if constexpr (std::is_same_v<T, float>) return TypeId::FLOAT32;
        else if constexpr (std::is_same_v<T, double>) return TypeId::FLOAT64;
        else if constexpr (std::is_same_v<T, Rational>) return TypeId::RATIONAL;
        else if constexpr (std::is_same_v<T, LocalizedString>) return TypeId::LSTR;
        else if constexpr (std::is_same_v<T, std::string>) return TypeId::STR;
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) return TypeId::BYTES;
        else if constexpr (std::is_same_v<T, std::shared_ptr<ValueArray>>) return TypeId::ARRAY;
        else if constexpr (std::is_same_v<T, Struct>) return TypeId::STRUCT;
        else if constexpr (std::is_same_v<T, Timestamp>) return TypeId::TS64;
        else if constexpr (std::is_same_v<T, Uuid>) return TypeId::UUID;
        else return TypeId::BYTES; }, v);
    }

    static void serialize_value(wk::ByteWriter &w, const Value &v);

    static void serialize_value(wk::ByteWriter &w, const Value &v)
    {
        std::visit([&w](const auto &val)
                   {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, uint8_t>) {
            w.write_u8(val);
        } else if constexpr (std::is_same_v<T, uint16_t>) {
            w.write_u16(val);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            w.write_u32(val);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            w.write_u64(val);
        } else if constexpr (std::is_same_v<T, int32_t>) {
            w.write_i32(val);
        } else if constexpr (std::is_same_v<T, float>) {
            w.write_f32(val);
        } else if constexpr (std::is_same_v<T, double>) {
            w.write_f64(val);
        } else if constexpr (std::is_same_v<T, Rational>) {
            w.write_i32(val.numerator);
            w.write_i32(val.denominator);
        } else if constexpr (std::is_same_v<T, LocalizedString>) {

            w.write_u8(static_cast<uint8_t>(val.lang.size()));
            w.write_str(val.lang);
            w.write_str(val.text);
        } else if constexpr (std::is_same_v<T, std::string>) {
            w.write_str(val);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            w.write_bytes(val);
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ValueArray>>) {
            if (val) {
                w.write_u16(static_cast<uint16_t>(val->elements.size()));
                if (!val->elements.empty()) {
                    w.write_u8(static_cast<uint8_t>(type_id_from_value(val->elements[0])));
                    for (const auto& elem : val->elements) {
                        serialize_value(w, elem);
                    }
                } else {
                    w.write_u8(static_cast<uint8_t>(TypeId::UINT8));
                }
            } else {
                w.write_u16(0);
                w.write_u8(static_cast<uint8_t>(TypeId::UINT8));
            }
        } else if constexpr (std::is_same_v<T, Struct>) {
            w.write_u16(static_cast<uint16_t>(val.fields.size()));
            for (const auto& field : val.fields) {
                w.write_u8(static_cast<uint8_t>(field.ns));
                w.write_u16(field.tag);
                TypeId tid = type_id_from_value(field.value);
                w.write_u8(static_cast<uint8_t>(tid));


                wk::ByteWriter val_w;
                serialize_value(val_w, field.value);
                auto val_data = val_w.finish();
                w.write_u32(static_cast<uint32_t>(val_data.size()));
                w.write_bytes(val_data);
            }
        } else if constexpr (std::is_same_v<T, Timestamp>) {
            w.write_u64(val.microseconds);
        } else if constexpr (std::is_same_v<T, Uuid>) {
            w.write_bytes({val.bytes, 16});
        } }, v);
    }

    static std::expected<Value, MetaError> parse_value(
        wk::ByteReader &r, TypeId type, uint32_t value_size);

    static std::expected<Value, MetaError> parse_value(
        wk::ByteReader &r, TypeId type, uint32_t value_size)
    {

        switch (type)
        {
        case TypeId::UINT8:
        {
            auto v = r.read_u8();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::UINT16:
        {
            auto v = r.read_u16();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::UINT32:
        {
            auto v = r.read_u32();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::UINT64:
        {
            auto v = r.read_u64();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::INT32:
        {
            auto v = r.read_i32();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::FLOAT32:
        {
            auto v = r.read_f32();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::FLOAT64:
        {
            auto v = r.read_f64();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{*v};
        }
        case TypeId::RATIONAL:
        {
            auto n = r.read_i32();
            auto d = r.read_i32();
            if (!n || !d)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{Rational{*n, *d}};
        }
        case TypeId::LSTR:
        {
            // value_size must hold at least the 1-byte language length;
            // otherwise the text-length subtraction below underflows.
            if (value_size < 1)
                return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                 "LSTR value_size is 0"});
            auto lang_len = r.read_u8();
            if (!lang_len)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            auto lang = r.read_str(*lang_len);
            if (!lang)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            if (value_size < 1 + static_cast<uint32_t>(*lang_len))
                return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                 "LSTR value_size smaller than language tag"});
            uint32_t text_len = value_size - 1 - *lang_len;
            auto text = r.read_str(text_len);
            if (!text)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{LocalizedString{std::move(*lang), std::move(*text)}};
        }
        case TypeId::STR:
        {
            auto s = r.read_str(value_size);
            if (!s)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{std::move(*s)};
        }
        case TypeId::BYTES:
        {
            auto b = r.read_bytes(value_size);
            if (!b)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{std::vector<uint8_t>(b->begin(), b->end())};
        }
        case TypeId::ARRAY:
        {
            // An ARRAY value is at least 3 bytes (count + element type);
            // otherwise the remaining-bytes subtraction below underflows.
            if (value_size < 3)
                return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                 "ARRAY value_size smaller than 3"});
            const size_t value_start = r.position();
            auto count = r.read_u16();
            if (!count)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            auto elem_type = r.read_u8();
            if (!elem_type)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});

            auto arr = std::make_shared<ValueArray>();
            arr->elements.reserve(*count);

            TypeId et = static_cast<TypeId>(*elem_type);
            uint32_t remaining = value_size - 3;
            uint32_t per_elem = (*count > 0) ? remaining / *count : 0;

            for (uint16_t i = 0; i < *count; i++)
            {
                auto elem = parse_value(r, et, per_elem);
                if (!elem)
                    return std::unexpected(elem.error());
                arr->elements.push_back(std::move(*elem));
            }
            // Variable-length elements split the value evenly, so the
            // elements may consume fewer bytes than declared (the
            // remainder is padding, e.g. unequal-length strings). Skip
            // to the declared end so the caller's framing stays exact.
            r.seek(value_start + value_size);
            return Value{std::move(arr)};
        }
        case TypeId::STRUCT:
        {
            auto field_count = r.read_u16();
            if (!field_count)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});

            Struct s;
            for (uint16_t i = 0; i < *field_count; i++)
            {
                const size_t field_start = r.position();
                auto ns = r.read_u8();
                auto tag = r.read_u16();
                auto tid = r.read_u8();
                auto vsize = r.read_u32();
                if (!ns || !tag || !tid || !vsize)
                {
                    return std::unexpected(MetaError{MetaErrorCode::ParseError});
                }

                // A nested field is framed exactly like a top-level
                // entry: its declared size must fit in the chunk, and
                // its value must consume exactly that many bytes.
                const size_t field_end = field_start + 8 + static_cast<size_t>(*vsize);
                if (field_end > r.size())
                {
                    return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                     "struct field extends past chunk"});
                }

                auto val = parse_value(r, static_cast<TypeId>(*tid), *vsize);
                if (!val)
                {
                    return std::unexpected(val.error());
                }
                if (r.position() != field_end)
                {
                    return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                     "struct field value size mismatch"});
                }

                Entry entry;
                entry.ns = static_cast<Namespace>(*ns);
                entry.tag = *tag;
                entry.value = std::move(*val);
                s.fields.push_back(std::move(entry));
            }
            return Value{std::move(s)};
        }
        case TypeId::TS64:
        {
            auto v = r.read_u64();
            if (!v)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            return Value{Timestamp{*v}};
        }
        case TypeId::UUID:
        {
            auto b = r.read_bytes(16);
            if (!b)
                return std::unexpected(MetaError{MetaErrorCode::ParseError});
            Uuid uuid;
            std::memcpy(uuid.bytes, b->data(), 16);
            return Value{uuid};
        }
        default:
            return std::unexpected(MetaError{MetaErrorCode::UnknownType,
                                             "unknown type id " + std::to_string(static_cast<int>(type))});
        }
    }

    const Entry *MetaBlock::find_entry(Namespace ns, uint16_t tag) const
    {
        for (const auto &e : entries)
        {
            if (e.ns == ns && e.tag == tag)
                return &e;
        }
        return nullptr;
    }

    Entry *MetaBlock::find_entry_mut(Namespace ns, uint16_t tag)
    {
        for (auto &e : entries)
        {
            if (e.ns == ns && e.tag == tag)
                return &e;
        }
        return nullptr;
    }

    auto MetaBlock::get(Namespace ns, uint16_t tag) const
        -> std::expected<const Value *, MetaError>
    {
        auto *e = find_entry(ns, tag);
        if (!e)
            return std::unexpected(MetaError{MetaErrorCode::TagNotFound});
        return &e->value;
    }

    auto MetaBlock::set(Namespace ns, uint16_t tag, Value v)
        -> std::expected<void, MetaError>
    {
        auto *e = find_entry_mut(ns, tag);
        if (e)
        {
            e->value = std::move(v);
        }
        else
        {
            entries.push_back({ns, tag, std::move(v), {}, false});
        }
        return {};
    }

    auto MetaBlock::remove(Namespace ns, uint16_t tag) -> bool
    {
        auto it = std::remove_if(entries.begin(), entries.end(),
                                 [ns, tag](const Entry &e)
                                 { return e.ns == ns && e.tag == tag; });
        if (it != entries.end())
        {
            entries.erase(it, entries.end());
            return true;
        }
        return false;
    }

    auto MetaBlock::get_geo_lat() const -> std::expected<double, MetaError>
    {
        auto r = get(Namespace::Geo, geo::LAT);
        if (!r)
            return std::unexpected(r.error());
        if (auto *v = std::get_if<double>(*r))
            return *v;
        return std::unexpected(MetaError{MetaErrorCode::TypeMismatch, "expected FLOAT64"});
    }

    auto MetaBlock::get_geo_lon() const -> std::expected<double, MetaError>
    {
        auto r = get(Namespace::Geo, geo::LON);
        if (!r)
            return std::unexpected(r.error());
        if (auto *v = std::get_if<double>(*r))
            return *v;
        return std::unexpected(MetaError{MetaErrorCode::TypeMismatch, "expected FLOAT64"});
    }

    auto MetaBlock::get_capture_ts() const -> std::expected<Timestamp, MetaError>
    {
        auto r = get(Namespace::Time, time_tags::CAPTURE_UTC);
        if (!r)
        {
            r = get(Namespace::Geo, geo::CAPTURE_TS);
        }
        if (!r)
            return std::unexpected(r.error());
        if (auto *v = std::get_if<Timestamp>(*r))
            return *v;
        return std::unexpected(MetaError{MetaErrorCode::TypeMismatch, "expected TS64"});
    }

    auto MetaBlock::get_title(std::string_view lang_bcp47) const
        -> std::expected<std::string, MetaError>
    {
        auto r = get(Namespace::Content, content::TITLE);
        if (!r)
            return std::unexpected(r.error());

        if (auto *v = std::get_if<LocalizedString>(*r))
        {
            if (lang_bcp47.empty() || v->lang == lang_bcp47)
            {
                return v->text;
            }
        }
        if (auto *v = std::get_if<std::string>(*r))
        {
            return *v;
        }
        return std::unexpected(MetaError{MetaErrorCode::TagNotFound, "title not found for lang"});
    }

    auto MetaBlock::get_regions() const -> std::vector<RegionAnnotation>
    {
        std::vector<RegionAnnotation> regions;

        for (const auto &e : entries)
        {
            if (e.ns == Namespace::Region)
            {
                if (auto *s = std::get_if<Struct>(&e.value))
                {
                    RegionAnnotation ann;
                    // String-ish fields accept both STR and LSTR, like
                    // get_title() does.
                    auto string_field = [](const Value &v) -> const std::string *
                    {
                        if (auto *ls = std::get_if<LocalizedString>(&v))
                            return &ls->text;
                        return std::get_if<std::string>(&v);
                    };
                    for (const auto &f : s->fields)
                    {
                        if (f.tag == region::NAME)
                        {
                            if (auto *t = string_field(f.value))
                                ann.name = *t;
                        }
                        else if (f.tag == region::TYPE)
                        {
                            if (auto *v = std::get_if<uint8_t>(&f.value))
                                ann.type = *v;
                        }
                        else if (f.tag == region::ROLE)
                        {
                            if (auto *t = string_field(f.value))
                                ann.role = *t;
                        }
                        else if (f.tag == region::RECT_NORM)
                        {
                            // Normalized rect: ARRAY of 4 FLOAT32
                            // (x, y, w, h), see SPEC section 7.6.
                            if (auto *a = std::get_if<std::shared_ptr<ValueArray>>(&f.value))
                            {
                                if (*a && (*a)->elements.size() == 4)
                                {
                                    float *coords[4] = {&ann.x, &ann.y, &ann.w, &ann.h};
                                    for (size_t k = 0; k < 4; k++)
                                    {
                                        if (auto *v = std::get_if<float>(&(*a)->elements[k]))
                                            *coords[k] = *v;
                                    }
                                }
                            }
                        }
                        else if (f.tag == region::CONFIDENCE)
                        {
                            if (auto *v = std::get_if<float>(&f.value))
                                ann.confidence = *v;
                        }
                        else if (f.tag == region::PERSON_NAME)
                        {
                            if (auto *t = string_field(f.value))
                                ann.person_name = *t;
                        }
                        else if (f.tag == region::PERSON_ID)
                        {
                            if (auto *v = std::get_if<Uuid>(&f.value))
                                ann.person_id = *v;
                        }
                        else if (f.tag == region::OBJECT_CLASS)
                        {
                            if (auto *t = string_field(f.value))
                                ann.object_class = *t;
                        }
                        else if (f.tag == region::CREATOR)
                        {
                            if (auto *t = string_field(f.value))
                                ann.creator = *t;
                        }
                    }
                    regions.push_back(std::move(ann));
                }
            }
        }

        return regions;
    }

    auto MetaBlock::get_license() const -> std::expected<std::string, MetaError>
    {
        auto r = get(Namespace::Rights, rights::LICENSE_SPDX);
        if (!r)
            return std::unexpected(r.error());
        if (auto *v = std::get_if<std::string>(*r))
            return *v;
        return std::unexpected(MetaError{MetaErrorCode::TypeMismatch});
    }

    auto MetaBlock::serialize() const -> std::vector<uint8_t>
    {
        wk::ByteWriter w;

        w.write_u8(0x01);
        w.write_u16(static_cast<uint16_t>(entries.size()));

        for (const auto &entry : entries)
        {
            if (entry.is_opaque)
            {

                w.write_bytes(entry.opaque_data);
                continue;
            }

            w.write_u8(static_cast<uint8_t>(entry.ns));
            w.write_u16(entry.tag);

            TypeId tid = type_id_from_value(entry.value);
            w.write_u8(static_cast<uint8_t>(tid));

            wk::ByteWriter val_w;
            serialize_value(val_w, entry.value);
            auto val_data = val_w.finish();

            w.write_u32(static_cast<uint32_t>(val_data.size()));
            w.write_bytes(val_data);
        }

        return w.finish();
    }

    auto MetaBlock::parse(std::span<const uint8_t> data)
        -> std::expected<MetaBlock, MetaError>
    {

        wk::ByteReader r(data);
        MetaBlock block;

        auto ver = r.read_u8();
        if (!ver)
            return std::unexpected(MetaError{MetaErrorCode::ParseError, "truncated"});
        if (*ver != 0x01)
        {
            return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                             "unsupported wkmeta version"});
        }

        auto count = r.read_u16();
        if (!count)
            return std::unexpected(MetaError{MetaErrorCode::ParseError});

        block.entries.reserve(*count);

        for (uint16_t i = 0; i < *count; i++)
        {
            size_t entry_start = r.position();

            auto ns_r = r.read_u8();
            auto tag_r = r.read_u16();
            auto tid_r = r.read_u8();
            auto vsize_r = r.read_u32();

            if (!ns_r || !tag_r || !tid_r || !vsize_r)
            {
                return std::unexpected(MetaError{MetaErrorCode::ParseError, "truncated entry"});
            }

            // value_size is authoritative for framing (SPEC section 7.2):
            // the entry occupies exactly 8 header + value_size bytes.
            // An entry claiming bytes past the chunk is truncated, not
            // preservable -- reject the chunk instead of keeping a
            // zero-filled placeholder for it.
            const size_t entry_end = entry_start + 8 + static_cast<size_t>(*vsize_r);
            if (entry_end > r.size())
            {
                return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                 "entry extends past META chunk"});
            }

            TypeId tid = static_cast<TypeId>(*tid_r);

            auto val = parse_value(r, tid, *vsize_r);

            Entry entry;
            entry.ns = static_cast<Namespace>(*ns_r);
            entry.tag = *tag_r;

            if (val)
            {
                // The value must consume exactly its declared size;
                // anything else means trailing garbage (or a short read)
                // inside the entry, and the next entry would start at
                // the wrong offset.
                if (r.position() != entry_end)
                {
                    return std::unexpected(MetaError{MetaErrorCode::ParseError,
                                                     "entry value size mismatch"});
                }
                entry.value = std::move(*val);
            }
            else
            {
                // Unparseable entry (unknown type, or a malformed value
                // for a known type): preserve it verbatim so a later
                // serialize() writes back exactly the same bytes
                // (SPEC section 10 rule 7). Reposition the reader at
                // the declared entry end -- parse_value may have
                // consumed bytes of the following entries before
                // failing, which used to desync the rest of the chunk.
                entry.is_opaque = true;
                entry.opaque_data.assign(data.begin() + static_cast<std::ptrdiff_t>(entry_start),
                                         data.begin() + static_cast<std::ptrdiff_t>(entry_end));
                r.seek(entry_end);
                entry.value = std::vector<uint8_t>{};
            }

            block.entries.push_back(std::move(entry));
        }

        return block;
    }

    static std::string json_escape(std::string_view s)
    {
        std::string result;
        result.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += c;
                break;
            }
        }
        return result;
    }

    static std::string xml_escape(std::string_view s)
    {
        std::string result;
        result.reserve(s.size());
        for (char c : s)
        {
            switch (c)
            {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '"':
                result += "&quot;";
                break;
            case '\'':
                result += "&apos;";
                break;
            default:
                result += c;
                break;
            }
        }
        return result;
    }

    // Reverse of xml_escape: the five named entities plus numeric
    // character references (&#NN; / &#xHH;), the latter re-encoded as
    // UTF-8. Anything unrecognized is left verbatim.
    static std::string xml_unescape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '&')
            {
                auto semi = s.find(';', i);
                if (semi != std::string_view::npos && semi - i <= 10)
                {
                    std::string_view ent = s.substr(i + 1, semi - i - 1);
                    char single = 0;
                    if (ent == "amp")
                        single = '&';
                    else if (ent == "lt")
                        single = '<';
                    else if (ent == "gt")
                        single = '>';
                    else if (ent == "quot")
                        single = '"';
                    else if (ent == "apos")
                        single = '\'';
                    if (single != 0)
                    {
                        out += single;
                        i = semi;
                        continue;
                    }
                    if (ent.size() > 1 && ent[0] == '#')
                    {
                        unsigned long cp = 0;
                        bool ok = false;
                        if (ent[1] == 'x' || ent[1] == 'X')
                        {
                            auto r = std::from_chars(ent.data() + 2, ent.data() + ent.size(), cp, 16);
                            ok = r.ec == std::errc();
                        }
                        else
                        {
                            auto r = std::from_chars(ent.data() + 1, ent.data() + ent.size(), cp);
                            ok = r.ec == std::errc();
                        }
                        if (ok && cp <= 0x10FFFF)
                        {
                            if (cp < 0x80)
                                out += static_cast<char>(cp);
                            else if (cp < 0x800)
                            {
                                out += static_cast<char>(0xC0 | (cp >> 6));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            else if (cp < 0x10000)
                            {
                                out += static_cast<char>(0xE0 | (cp >> 12));
                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            else
                            {
                                out += static_cast<char>(0xF0 | (cp >> 18));
                                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            i = semi;
                            continue;
                        }
                    }
                }
            }
            out += s[i];
        }
        return out;
    }

    // Text content of a STR or LSTR value, or empty for anything else.
    static std::string_view meta_text(const Value &v)
    {
        if (auto *ls = std::get_if<LocalizedString>(&v))
            return ls->text;
        if (auto *s = std::get_if<std::string>(&v))
            return *s;
        return {};
    }

    static const Value *meta_find(const MetaBlock &b, Namespace ns, uint16_t tag)
    {
        auto r = b.get(ns, tag);
        return r ? *r : nullptr;
    }

    static std::string namespace_name(Namespace ns)
    {
        switch (ns)
        {
        case Namespace::Capture:
            return "capture";
        case Namespace::Geo:
            return "geo";
        case Namespace::Time:
            return "time";
        case Namespace::Rights:
            return "rights";
        case Namespace::Content:
            return "content";
        case Namespace::Anim:
            return "anim";
        case Namespace::Region:
            return "region";
        case Namespace::Device:
            return "device";
        case Namespace::Rating:
            return "rating";
        case Namespace::Custom:
            return "custom";
        case Namespace::ProvRef:
            return "provRef";
        default:
            return "unknown_" + std::to_string(static_cast<int>(ns));
        }
    }

    static std::string value_to_json(const Value &v, int indent = 2);

    static std::string value_to_json(const Value &v, int indent)
    {
        return std::visit([indent](const auto &val) -> std::string
                          {
        using T = std::decay_t<decltype(val)>;

        if constexpr (std::is_same_v<T, uint8_t>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, uint16_t>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, int32_t>) {
            return std::to_string(val);
        } else if constexpr (std::is_same_v<T, float>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(val));
            return buf;
        } else if constexpr (std::is_same_v<T, double>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.10g", val);
            return buf;
        } else if constexpr (std::is_same_v<T, Rational>) {
            return "{\"numerator\":" + std::to_string(val.numerator)
                 + ",\"denominator\":" + std::to_string(val.denominator) + "}";
        } else if constexpr (std::is_same_v<T, LocalizedString>) {
            return "{\"" + json_escape(val.lang.empty() ? "default" : val.lang)
                 + "\":\"" + json_escape(val.text) + "\"}";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + json_escape(val) + "\"";
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            std::string s = "\"<";
            s += std::to_string(val.size());
            s += " bytes>\"";
            return s;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<ValueArray>>) {
            if (!val || val->elements.empty()) return "[]";
            std::string s = "[";
            for (size_t i = 0; i < val->elements.size(); i++) {
                if (i > 0) s += ",";
                s += value_to_json(val->elements[i], indent + 2);
            }
            s += "]";
            return s;
        } else if constexpr (std::is_same_v<T, Struct>) {
            if (val.fields.empty()) return "{}";
            std::string s = "{";
            for (size_t i = 0; i < val.fields.size(); i++) {
                if (i > 0) s += ",";
                s += "\"" + std::to_string(val.fields[i].tag) + "\":";
                s += value_to_json(val.fields[i].value, indent + 2);
            }
            s += "}";
            return s;
        } else if constexpr (std::is_same_v<T, Timestamp>) {
            return "\"" + val.to_iso8601() + "\"";
        } else if constexpr (std::is_same_v<T, Uuid>) {
            return "\"" + val.to_string() + "\"";
        } else {
            return "null";
        } }, v);
    }

    auto MetaBlock::to_json() const -> std::string
    {
        std::string json = "{\n";

        std::map<Namespace, std::vector<const Entry *>> grouped;
        for (const auto &e : entries)
        {
            grouped[e.ns].push_back(&e);
        }

        bool first_ns = true;
        for (const auto &[ns, ns_entries] : grouped)
        {
            if (!first_ns)
                json += ",\n";
            first_ns = false;

            json += "  \"" + namespace_name(ns) + "\": {\n";

            bool first_entry = true;
            for (const auto *e : ns_entries)
            {
                if (!first_entry)
                    json += ",\n";
                first_entry = false;

                char tag_buf[8];
                std::snprintf(tag_buf, sizeof(tag_buf), "0x%04X", e->tag);

                json += "    \"" + std::string(tag_buf) + "\": ";
                if (e->is_opaque)
                {
                    json += "\"<opaque " + std::to_string(e->opaque_data.size()) + " bytes>\"";
                }
                else
                {
                    json += value_to_json(e->value, 4);
                }
            }
            json += "\n  }";
        }

        json += "\n}\n";
        return json;
    }

    auto MetaBlock::from_exif(std::span<const uint8_t> exif_blob)
        -> std::expected<MetaBlock, MetaError>
    {
        return parse_exif_blob(exif_blob);
    }

    auto MetaBlock::from_xmp(std::string_view xmp_xml)
        -> std::expected<MetaBlock, MetaError>
    {
        MetaBlock block;

        auto extract_tag = [&](std::string_view tag_name) -> std::string
        {
            std::string open = "<" + std::string(tag_name) + ">";
            std::string close = "</" + std::string(tag_name) + ">";
            auto start = xmp_xml.find(open);
            if (start == std::string_view::npos)
                return "";
            start += open.size();
            auto end = xmp_xml.find(close, start);
            if (end == std::string_view::npos)
                return "";
            return std::string(xmp_xml.substr(start, end - start));
        };

        // XMP localized/sequence values are wrapped in rdf:Alt / rdf:Seq
        // with <rdf:li> items; unwrap to the first item's text (and its
        // xml:lang, if any). Plain <tag>text</tag> content passes
        // through unchanged. Returns {text, lang}.
        auto unwrap_value = [](std::string_view content) -> std::pair<std::string, std::string>
        {
            auto li_open = content.find("<rdf:li");
            if (li_open == std::string_view::npos)
                return {xml_unescape(content), ""};
            auto gt = content.find('>', li_open);
            if (gt == std::string_view::npos)
                return {xml_unescape(content), ""};
            std::string lang;
            auto lang_attr = content.find("xml:lang=\"", li_open);
            if (lang_attr != std::string_view::npos && lang_attr < gt)
            {
                auto lang_start = lang_attr + 10;
                auto lang_end = content.find('"', lang_start);
                if (lang_end != std::string_view::npos && lang_end < gt)
                    lang = content.substr(lang_start, lang_end - lang_start);
            }
            if (lang == "x-default")
                lang = "";
            auto li_close = content.find("</rdf:li>", gt);
            if (li_close == std::string_view::npos)
                return {xml_unescape(content), lang};
            return {xml_unescape(content.substr(gt + 1, li_close - gt - 1)), lang};
        };

        auto [title, title_lang] = unwrap_value(extract_tag("dc:title"));
        if (!title.empty())
        {
            block.set(Namespace::Content, content::TITLE,
                      LocalizedString{title_lang, title});
        }

        auto [desc, desc_lang] = unwrap_value(extract_tag("dc:description"));
        if (!desc.empty())
        {
            block.set(Namespace::Content, content::DESCRIPTION,
                      LocalizedString{desc_lang, desc});
        }

        auto [creator, creator_lang] = unwrap_value(extract_tag("dc:creator"));
        if (!creator.empty())
        {
            block.set(Namespace::Rights, rights::CREATOR,
                      LocalizedString{creator_lang, creator});
        }

        auto [copyright, copyright_lang] = unwrap_value(extract_tag("dc:rights"));
        if (!copyright.empty())
        {
            block.set(Namespace::Rights, rights::COPYRIGHT,
                      LocalizedString{copyright_lang, copyright});
        }

        auto [license_terms, license_terms_lang] = unwrap_value(extract_tag("xmpRights:UsageTerms"));
        if (!license_terms.empty())
        {
            (void)license_terms_lang;
            block.set(Namespace::Rights, rights::LICENSE_SPDX, license_terms);
        }

        auto license_url = unwrap_value(extract_tag("xmpRights:WebStatement")).first;
        if (!license_url.empty())
        {
            block.set(Namespace::Rights, rights::LICENSE_URL, license_url);
        }

        auto make = unwrap_value(extract_tag("tiff:Make")).first;
        if (!make.empty())
        {
            block.set(Namespace::Capture, capture::MAKE, make);
        }

        auto model = unwrap_value(extract_tag("tiff:Model")).first;
        if (!model.empty())
        {
            block.set(Namespace::Capture, capture::MODEL, model);
        }

        auto software = unwrap_value(extract_tag("xmp:CreatorTool")).first;
        if (!software.empty())
        {
            block.set(Namespace::Capture, capture::SOFTWARE, software);
        }

        return block;
    }

    auto MetaBlock::to_exif() const -> std::vector<uint8_t>
    {
        // Minimal little-endian TIFF writer for the fields
        // parse_exif_blob() understands (IFD0 Make/Model/Software, Exif
        // ISO, GPS lat/lon/alt/date/time). Only tags present in the block
        // are written; a block with none of them produces an empty
        // vector (there is nothing to write).
        const Value *make_v = meta_find(*this, Namespace::Capture, capture::MAKE);
        const Value *model_v = meta_find(*this, Namespace::Capture, capture::MODEL);
        const Value *software_v = meta_find(*this, Namespace::Capture, capture::SOFTWARE);
        const std::string_view make = make_v ? meta_text(*make_v) : std::string_view{};
        const std::string_view model = model_v ? meta_text(*model_v) : std::string_view{};
        const std::string_view software = software_v ? meta_text(*software_v) : std::string_view{};

        std::optional<uint32_t> iso;
        if (const Value *v = meta_find(*this, Namespace::Capture, capture::ISO))
        {
            if (auto *u = std::get_if<uint32_t>(v))
            {
                if (*u <= 0xFFFF)
                    iso = *u;
            }
        }

        std::optional<double> lat;
        std::optional<double> lon;
        if (auto r = get_geo_lat())
            lat = *r;
        if (auto r = get_geo_lon())
            lon = *r;

        std::optional<float> alt;
        if (const Value *v = meta_find(*this, Namespace::Geo, geo::ALT))
        {
            if (auto *f = std::get_if<float>(v))
                alt = *f;
            else if (auto *d = std::get_if<double>(v))
                alt = static_cast<float>(*d);
        }

        std::optional<Timestamp> ts;
        if (auto r = get_capture_ts())
            ts = *r;

        const bool has_gps = lat || lon || alt || ts;
        if (make.empty() && model.empty() && software.empty() && !iso && !has_gps)
            return {};

        constexpr uint16_t kAscii = 2;
        constexpr uint16_t kShort = 3;
        constexpr uint16_t kLong = 4;
        constexpr uint16_t kRational = 5;
        constexpr uint16_t kByte = 1;

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

        // IFD0 entries, ascending tag order: Make, Model, Software,
        // ExifIFD pointer, GPSIFD pointer.
        uint16_t ifd0_count = 0;
        if (!make.empty())
            ifd0_count++;
        if (!model.empty())
            ifd0_count++;
        if (!software.empty())
            ifd0_count++;
        if (iso)
            ifd0_count++;
        if (has_gps)
            ifd0_count++;

        uint16_t gps_count = 0;
        if (lat)
            gps_count += 2;  // LatRef + Lat
        if (lon)
            gps_count += 2;  // LonRef + Lon
        if (alt)
            gps_count += 2;  // AltRef + Alt
        if (ts)
            gps_count += 2;  // Time + Date

        const uint32_t ifd0_offset = 8;
        const uint32_t ifd0_size = 2 + 12u * ifd0_count + 4;
        const uint32_t exif_offset = ifd0_offset + ifd0_size;
        const uint32_t exif_size = iso ? (2 + 12u + 4) : 0;
        const uint32_t gps_offset = exif_offset + exif_size;
        const uint32_t gps_size = has_gps ? (2 + 12u * gps_count + 4) : 0;
        uint32_t data_offset = gps_offset + gps_size;

        // Data blobs (strings > 4 bytes, rationals, date string) are
        // laid out after all IFDs; assign their offsets first.
        auto place_blob = [&](uint32_t size) {
            uint32_t off = data_offset;
            data_offset += size;
            return off;
        };
        uint32_t make_off = 0, model_off = 0, software_off = 0;
        if (!make.empty() && make.size() + 1 > 4)
            make_off = place_blob(static_cast<uint32_t>(make.size() + 1));
        if (!model.empty() && model.size() + 1 > 4)
            model_off = place_blob(static_cast<uint32_t>(model.size() + 1));
        if (!software.empty() && software.size() + 1 > 4)
            software_off = place_blob(static_cast<uint32_t>(software.size() + 1));
        uint32_t lat_off = 0, lon_off = 0, alt_off = 0, time_off = 0, date_off = 0;
        if (lat)
            lat_off = place_blob(24);
        if (lon)
            lon_off = place_blob(24);
        if (alt)
            alt_off = place_blob(8);
        if (ts)
        {
            time_off = place_blob(24);
            date_off = place_blob(11);
        }

        auto write_entry = [](wk::ByteWriter &w, uint16_t tag, uint16_t type,
                              uint32_t count, uint32_t value_or_offset)
        {
            w.write_u16(tag);
            w.write_u16(type);
            w.write_u32(count);
            w.write_u32(value_or_offset);
        };
        auto inline_bytes = [](std::string_view s) {
            uint32_t v = 0;
            for (size_t i = 0; i < s.size() && i < 4; i++)
                v |= static_cast<uint32_t>(static_cast<uint8_t>(s[i])) << (8 * i);
            return v;
        };
        auto to_dms = [](double v) {
            double a = std::fabs(v);
            uint32_t d = static_cast<uint32_t>(a);
            double mf = (a - d) * 60.0;
            uint32_t m = static_cast<uint32_t>(mf);
            double s = (mf - m) * 60.0;
            uint32_t s_num = static_cast<uint32_t>(std::llround(s * 1000000.0));
            if (s_num >= 60000000u)
            {
                s_num -= 60000000u;
                if (++m >= 60)
                {
                    m -= 60;
                    d += 1;
                }
            }
            return std::array<std::pair<uint32_t, uint32_t>, 3>{
                std::pair{d, 1u}, std::pair{m, 1u}, std::pair{s_num, 1000000u}};
        };

        wk::ByteWriter w;
        w.write_u8('I');
        w.write_u8('I');
        w.write_u16(42);
        w.write_u32(ifd0_offset);

        w.write_u16(ifd0_count);
        if (!make.empty())
            write_entry(w, kTagMake, kAscii, static_cast<uint32_t>(make.size() + 1),
                        make_off ? make_off : inline_bytes(make));
        if (!model.empty())
            write_entry(w, kTagModel, kAscii, static_cast<uint32_t>(model.size() + 1),
                        model_off ? model_off : inline_bytes(model));
        if (!software.empty())
            write_entry(w, kTagSoftware, kAscii, static_cast<uint32_t>(software.size() + 1),
                        software_off ? software_off : inline_bytes(software));
        if (iso)
            write_entry(w, kTagExifIfd, kLong, 1, exif_offset);
        if (has_gps)
            write_entry(w, kTagGpsIfd, kLong, 1, gps_offset);
        w.write_u32(0);

        if (iso)
        {
            w.write_u16(1);
            write_entry(w, kTagIso, kShort, 1, *iso);
            w.write_u32(0);
        }

        if (has_gps)
        {
            w.write_u16(gps_count);
            if (lat)
            {
                write_entry(w, kGpsLatRef, kAscii, 2,
                            static_cast<uint32_t>(*lat < 0 ? 'S' : 'N'));
                write_entry(w, kGpsLat, kRational, 3, lat_off);
            }
            if (lon)
            {
                write_entry(w, kGpsLonRef, kAscii, 2,
                            static_cast<uint32_t>(*lon < 0 ? 'W' : 'E'));
                write_entry(w, kGpsLon, kRational, 3, lon_off);
            }
            if (alt)
            {
                write_entry(w, kGpsAltRef, kByte, 1, *alt < 0 ? 1 : 0);
                write_entry(w, kGpsAlt, kRational, 1, alt_off);
            }
            if (ts)
            {
                write_entry(w, kGpsTime, kRational, 3, time_off);
                write_entry(w, kGpsDate, kAscii, 11, date_off);
            }
            w.write_u32(0);
        }

        // Data area, in the order the offsets were assigned.
        if (make_off)
        {
            w.write_str(make);
            w.write_u8(0);
        }
        if (model_off)
        {
            w.write_str(model);
            w.write_u8(0);
        }
        if (software_off)
        {
            w.write_str(software);
            w.write_u8(0);
        }
        if (lat)
        {
            for (auto [num, den] : to_dms(*lat))
            {
                w.write_u32(num);
                w.write_u32(den);
            }
        }
        if (lon)
        {
            for (auto [num, den] : to_dms(*lon))
            {
                w.write_u32(num);
                w.write_u32(den);
            }
        }
        if (alt)
        {
            uint32_t num = static_cast<uint32_t>(std::llround(std::fabs(*alt) * 1000.0));
            w.write_u32(num);
            w.write_u32(1000);
        }
        if (ts)
        {
            time_t secs = static_cast<time_t>(ts->microseconds / 1000000ULL);
            uint32_t usec = static_cast<uint32_t>(ts->microseconds % 1000000ULL);
            struct tm utc_tm;
#ifdef _WIN32
            gmtime_s(&utc_tm, &secs);
#else
            gmtime_r(&secs, &utc_tm);
#endif
            w.write_u32(static_cast<uint32_t>(utc_tm.tm_hour));
            w.write_u32(1);
            w.write_u32(static_cast<uint32_t>(utc_tm.tm_min));
            w.write_u32(1);
            w.write_u32(static_cast<uint32_t>(utc_tm.tm_sec) * 1000000u + usec);
            w.write_u32(1000000);
            char date_buf[16];
            std::strftime(date_buf, sizeof(date_buf), "%Y:%m:%d", &utc_tm);
            w.write_str(date_buf);
            w.write_u8(0);
        }

        return w.finish();
    }

    auto MetaBlock::to_xmp() const -> std::string
    {
        // A well-formed XMP packet (XMP Specification Part 1): packet
        // header, xmpmeta wrapper, RDF with the dc / xmp / xmpRights /
        // tiff namespaces, and the mapped fields. Values are XML-escaped
        // (never JSON-escaped). Localized values use rdf:Alt with an
        // xml:lang, multi-valued ones rdf:Seq, matching what from_xmp()
        // reads back.
        std::string xmp;
        xmp += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
        xmp += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"libwk\">\n";
        xmp += "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
        xmp += "<rdf:Description rdf:about=\"\"\n";
        xmp += "  xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n";
        xmp += "  xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
        xmp += "  xmlns:xmpRights=\"http://ns.adobe.com/xap/1.0/rights/\"\n";
        xmp += "  xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\">\n";

        auto lang_of = [](const Value *v) -> std::string
        {
            if (!v)
                return {};
            if (auto *ls = std::get_if<LocalizedString>(v))
                return ls->lang;
            return {};
        };
        auto alt = [&](std::string_view tag, const Value *v)
        {
            if (!v)
                return;
            std::string_view text = meta_text(*v);
            if (text.empty())
                return;
            std::string lang = lang_of(v);
            xmp += "  <" + std::string(tag) + "><rdf:Alt><rdf:li xml:lang=\"";
            xmp += xml_escape(lang.empty() ? "x-default" : lang);
            xmp += "\">" + xml_escape(text) + "</rdf:li></rdf:Alt></" + std::string(tag) + ">\n";
        };
        auto plain = [&](std::string_view tag, const Value *v)
        {
            if (!v)
                return;
            std::string_view text = meta_text(*v);
            if (text.empty())
                return;
            xmp += "  <" + std::string(tag) + ">" + xml_escape(text) + "</" + std::string(tag) + ">\n";
        };

        alt("dc:title", meta_find(*this, Namespace::Content, content::TITLE));
        alt("dc:description", meta_find(*this, Namespace::Content, content::DESCRIPTION));

        if (const Value *creator = meta_find(*this, Namespace::Rights, rights::CREATOR))
        {
            std::string_view text = meta_text(*creator);
            if (!text.empty())
            {
                xmp += "  <dc:creator><rdf:Seq><rdf:li>" + xml_escape(text) +
                       "</rdf:li></rdf:Seq></dc:creator>\n";
            }
        }

        alt("dc:rights", meta_find(*this, Namespace::Rights, rights::COPYRIGHT));
        alt("xmpRights:UsageTerms", meta_find(*this, Namespace::Rights, rights::LICENSE_SPDX));
        plain("xmpRights:WebStatement", meta_find(*this, Namespace::Rights, rights::LICENSE_URL));
        plain("tiff:Make", meta_find(*this, Namespace::Capture, capture::MAKE));
        plain("tiff:Model", meta_find(*this, Namespace::Capture, capture::MODEL));
        plain("xmp:CreatorTool", meta_find(*this, Namespace::Capture, capture::SOFTWARE));

        if (auto ts = get_capture_ts())
        {
            xmp += "  <xmp:CreateDate>" + ts->to_iso8601() + "</xmp:CreateDate>\n";
        }

        xmp += "</rdf:Description>\n";
        xmp += "</rdf:RDF>\n";
        xmp += "</x:xmpmeta>\n";
        xmp += "<?xpacket end=\"w\"?>\n";
        return xmp;
    }

}
