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
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wk::meta
{

    enum class MetaErrorCode : uint8_t
    {
        Ok = 0,
        ParseError,
        SerializeError,
        TagNotFound,
        TypeMismatch,
        InvalidNamespace,
        InvalidTag,
        InvalidValue,
        MalformedInput,
        UnknownType,
    };

    struct MetaError
    {
        MetaErrorCode code;
        std::string message;

        MetaError(MetaErrorCode c, std::string msg = {})
            : code(c), message(std::move(msg)) {}
    };

    enum class Namespace : uint8_t
    {
        Capture = 0x01,
        Geo = 0x02,
        Time = 0x03,
        Rights = 0x04,
        Content = 0x05,
        Anim = 0x06,
        Region = 0x07,
        Device = 0x08,
        Rating = 0x09,
        Custom = 0x0A,
        ProvRef = 0x0B,
    };

    enum class TypeId : uint8_t
    {
        UINT8 = 0x01,
        UINT16 = 0x02,
        UINT32 = 0x03,
        UINT64 = 0x04,
        INT32 = 0x05,
        FLOAT32 = 0x06,
        FLOAT64 = 0x07,
        RATIONAL = 0x08,
        LSTR = 0x09,
        STR = 0x0A,
        BYTES = 0x0B,
        ARRAY = 0x0C,
        STRUCT = 0x0D,
        TS64 = 0x0E,
        UUID = 0x0F,
    };

    struct Rational
    {
        int32_t numerator = 0;
        int32_t denominator = 1;

        [[nodiscard]] double to_double() const
        {
            return denominator != 0 ? static_cast<double>(numerator) / denominator : 0.0;
        }

        bool operator==(const Rational &) const = default;
    };

    struct LocalizedString
    {
        std::string lang;
        std::string text;

        bool operator==(const LocalizedString &) const = default;
    };

    struct Timestamp
    {
        uint64_t microseconds = 0;

        [[nodiscard]] std::string to_iso8601() const;
        static Timestamp from_iso8601(std::string_view s);

        bool operator==(const Timestamp &) const = default;
    };

    struct Uuid
    {
        uint8_t bytes[16] = {};

        [[nodiscard]] std::string to_string() const;
        static Uuid from_string(std::string_view s);
        static Uuid generate();

        bool operator==(const Uuid &) const = default;
    };

    struct Entry;
    struct ValueArray;

    struct Struct
    {
        std::vector<Entry> fields;
        bool operator==(const Struct &) const;
    };

    using Value = std::variant<
        uint8_t,
        uint16_t,
        uint32_t,
        uint64_t,
        int32_t,
        float,
        double,
        Rational,
        LocalizedString,
        std::string,
        std::vector<uint8_t>,
        std::shared_ptr<ValueArray>,
        Struct,
        Timestamp,
        Uuid>;

    struct ValueArray
    {
        std::vector<Value> elements;
        bool operator==(const ValueArray &o) const = default;
    };

    struct Entry
    {
        Namespace ns;
        uint16_t tag;
        Value value;

        std::vector<uint8_t> opaque_data;
        bool is_opaque = false;

        bool operator==(const Entry &) const = default;
    };

    namespace geo
    {
        constexpr uint16_t LAT = 0x0001;
        constexpr uint16_t LON = 0x0002;
        constexpr uint16_t ALT = 0x0003;
        constexpr uint16_t HPOS_ERR = 0x0004;
        constexpr uint16_t VPOS_ERR = 0x0005;
        constexpr uint16_t SPEED = 0x0006;
        constexpr uint16_t HEADING = 0x0007;
        constexpr uint16_t PITCH = 0x0008;
        constexpr uint16_t ROLL = 0x0009;
        constexpr uint16_t COUNTRY = 0x000A;
        constexpr uint16_t REGION = 0x000B;
        constexpr uint16_t CITY = 0x000C;
        constexpr uint16_t SUBLOC = 0x000D;
        constexpr uint16_t PLACE_NAME = 0x000E;
        constexpr uint16_t WHAT3WORDS = 0x000F;
        constexpr uint16_t PLUS_CODE = 0x0010;
        constexpr uint16_t CAPTURE_TS = 0x0011;
        constexpr uint16_t DEST_LAT = 0x0012;
        constexpr uint16_t DEST_LON = 0x0013;
    }

    namespace time_tags
    {
        constexpr uint16_t CAPTURE_UTC = 0x0001;
        constexpr uint16_t OFFSET_SEC = 0x0002;
        constexpr uint16_t CREATED_UTC = 0x0003;
        constexpr uint16_t MODIFIED_UTC = 0x0004;
        constexpr uint16_t DIGITIZED_UTC = 0x0005;
        constexpr uint16_t SUBSEC = 0x0006;
        constexpr uint16_t TIMEZONE_ID = 0x0007;
    }

    namespace capture
    {
        constexpr uint16_t MAKE = 0x0001;
        constexpr uint16_t MODEL = 0x0002;
        constexpr uint16_t SERIAL = 0x0003;
        constexpr uint16_t LENS_MAKE = 0x0004;
        constexpr uint16_t LENS_MODEL = 0x0005;
        constexpr uint16_t FOCAL_LEN = 0x0006;
        constexpr uint16_t FOCAL_35MM = 0x0007;
        constexpr uint16_t FNUMBER = 0x0008;
        constexpr uint16_t EXPOSURE_TIME = 0x0009;
        constexpr uint16_t ISO = 0x000A;
        constexpr uint16_t EV_COMP = 0x000B;
        constexpr uint16_t FLASH = 0x000C;
        constexpr uint16_t WHITE_BAL = 0x000D;
        constexpr uint16_t METERING = 0x000E;
        constexpr uint16_t SCENE_TYPE = 0x000F;
        constexpr uint16_t SHARPNESS = 0x0010;
        constexpr uint16_t SATURATION = 0x0011;
        constexpr uint16_t SOFTWARE = 0x0012;
        constexpr uint16_t UNIQUE_IMG_ID = 0x0013;
        constexpr uint16_t BURST_INDEX = 0x0014;
        constexpr uint16_t BURST_COUNT = 0x0015;
        constexpr uint16_t HDR_FRAMES = 0x0016;
    }

    namespace rights
    {
        constexpr uint16_t CREATOR = 0x0001;
        constexpr uint16_t CREATOR_ROLE = 0x0002;
        constexpr uint16_t COPYRIGHT = 0x0003;
        constexpr uint16_t LICENSE_SPDX = 0x0004;
        constexpr uint16_t LICENSE_URL = 0x0005;
        constexpr uint16_t CREDIT_LINE = 0x0006;
        constexpr uint16_t SOURCE = 0x0007;
        constexpr uint16_t INSTRUCTIONS = 0x0008;
        constexpr uint16_t EMBARGO_UTC = 0x0009;
    }

    namespace content
    {
        constexpr uint16_t TITLE = 0x0001;
        constexpr uint16_t DESCRIPTION = 0x0002;
        constexpr uint16_t ALT_TEXT = 0x0003;
        constexpr uint16_t KEYWORDS = 0x0004;
        constexpr uint16_t GENRE = 0x0005;
        constexpr uint16_t SUBJECT_CODES = 0x0006;
        constexpr uint16_t SCENE_CODES = 0x0007;
        constexpr uint16_t EVENT = 0x0008;
        constexpr uint16_t OBJECT_NAME = 0x0009;
        constexpr uint16_t CATEGORY = 0x000A;
        constexpr uint16_t LANGUAGE = 0x000B;
        constexpr uint16_t SMARTCROP = 0x000C;
    }

    namespace anim_tags
    {
        constexpr uint16_t TITLE = 0x0001;
        constexpr uint16_t DESCRIPTION = 0x0002;
        constexpr uint16_t CREATOR = 0x0003;
        constexpr uint16_t DURATION_MS = 0x0004;
        constexpr uint16_t FPS_TARGET = 0x0005;
        constexpr uint16_t CATEGORY = 0x0006;
        constexpr uint16_t SCENE_LABELS = 0x0007;
        constexpr uint16_t THUMB_TILE = 0x0008;
    }

    namespace region
    {
        constexpr uint16_t NAME = 0x0001;
        constexpr uint16_t TYPE = 0x0002;
        constexpr uint16_t ROLE = 0x0003;
        constexpr uint16_t RECT_NORM = 0x0004;
        constexpr uint16_t CONFIDENCE = 0x0005;
        constexpr uint16_t PERSON_NAME = 0x0006;
        constexpr uint16_t PERSON_ID = 0x0007;
        constexpr uint16_t OBJECT_CLASS = 0x0008;
        constexpr uint16_t CREATOR = 0x0009;
    }

    namespace rating
    {
        constexpr uint16_t STARS = 0x0001;
        constexpr uint16_t AUDIENCE = 0x0002;
        constexpr uint16_t NSFW = 0x0003;
        constexpr uint16_t AI_GENERATED = 0x0004;
        constexpr uint16_t QUALITY_SCORE = 0x0005;
        constexpr uint16_t CAPTION_SRC = 0x0006;
    }

    struct RegionAnnotation
    {
        std::string name;
        uint8_t type = 0;
        std::string role;
        float x = 0, y = 0, w = 0, h = 0;
        float confidence = 0;
        std::string person_name;
        Uuid person_id;
        std::string object_class;
        std::string creator;
    };

    class MetaBlock
    {
    public:
        std::vector<Entry> entries;

        [[nodiscard]] auto get(Namespace ns, uint16_t tag) const
            -> std::expected<const Value *, MetaError>;

        auto set(Namespace ns, uint16_t tag, Value v)
            -> std::expected<void, MetaError>;

        auto remove(Namespace ns, uint16_t tag) -> bool;

        [[nodiscard]] auto get_geo_lat() const -> std::expected<double, MetaError>;
        [[nodiscard]] auto get_geo_lon() const -> std::expected<double, MetaError>;
        [[nodiscard]] auto get_capture_ts() const -> std::expected<Timestamp, MetaError>;
        [[nodiscard]] auto get_title(std::string_view lang_bcp47 = "") const
            -> std::expected<std::string, MetaError>;
        [[nodiscard]] auto get_regions() const -> std::vector<RegionAnnotation>;
        // Sets one field of a region annotation (SPEC section 7.6).
        // The field is merged into the first REGION STRUCT entry; if
        // the block has none, a new STRUCT entry (tag 0x0001) is
        // created. Other STRUCT entries are left untouched. The tag
        // must be one of the region field tags (0x0001-0x0009).
        auto set_region_field(uint16_t tag, Value v) -> std::expected<void, MetaError>;
        [[nodiscard]] auto get_license() const -> std::expected<std::string, MetaError>;

        [[nodiscard]] auto serialize() const -> std::vector<uint8_t>;
        [[nodiscard]] static auto parse(std::span<const uint8_t> data)
            -> std::expected<MetaBlock, MetaError>;

        [[nodiscard]] static auto from_exif(std::span<const uint8_t> exif_blob)
            -> std::expected<MetaBlock, MetaError>;
        [[nodiscard]] static auto from_xmp(std::string_view xmp_xml)
            -> std::expected<MetaBlock, MetaError>;

        [[nodiscard]] auto to_exif() const -> std::vector<uint8_t>;
        [[nodiscard]] auto to_xmp() const -> std::string;
        [[nodiscard]] auto to_json() const -> std::string;

    private:
        [[nodiscard]] auto find_entry(Namespace ns, uint16_t tag) const
            -> const Entry *;
        [[nodiscard]] auto find_entry_mut(Namespace ns, uint16_t tag)
            -> Entry *;
    };

}
