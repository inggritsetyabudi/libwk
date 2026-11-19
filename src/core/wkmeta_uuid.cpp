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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

namespace wk::meta
{

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

}
