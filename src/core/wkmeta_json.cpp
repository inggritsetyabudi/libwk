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
#include <map>
#include <string>
#include <string_view>

namespace wk::meta
{

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

}
