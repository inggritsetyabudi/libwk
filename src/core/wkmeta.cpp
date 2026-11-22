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

#include "common.h"
#include <wk/wkmeta.hpp>

namespace wk::meta
{

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

    auto MetaBlock::set_region_field(uint16_t tag, Value v)
        -> std::expected<void, MetaError>
    {
        // #meta: one STRUCT per region (SPEC section 7.6); flat
        // REGION entries are not valid regions, so fields always
        // land in a STRUCT, never as flat entries.
        switch (tag)
        {
        case region::NAME:
        case region::TYPE:
        case region::ROLE:
        case region::RECT_NORM:
        case region::CONFIDENCE:
        case region::PERSON_NAME:
        case region::PERSON_ID:
        case region::OBJECT_CLASS:
        case region::CREATOR:
            break;
        default:
            return std::unexpected(
                MetaError{MetaErrorCode::InvalidTag, "not a region field tag"});
        }

        for (auto &e : entries)
        {
            if (e.ns != Namespace::Region || e.is_opaque)
                continue;
            auto *s = std::get_if<Struct>(&e.value);
            if (!s)
                continue;
            for (auto &f : s->fields)
            {
                if (f.tag == tag)
                {
                    f.value = std::move(v);
                    return {};
                }
            }
            s->fields.push_back({Namespace::Region, tag, std::move(v), {}, false});
            return {};
        }

        Struct s;
        s.fields.push_back({Namespace::Region, tag, std::move(v), {}, false});
        entries.push_back({Namespace::Region, region::NAME, std::move(s), {}, false});
        return {};
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

}
