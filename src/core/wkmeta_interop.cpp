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
#include <wk/wkmeta.hpp>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace wk::meta
{

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
