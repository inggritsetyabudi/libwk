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
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

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

}
