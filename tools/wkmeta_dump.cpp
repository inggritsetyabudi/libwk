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
#include "../src/common.h"
#include "../src/container.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: wkmeta-dump <input.wk>\n");
        return 1;
    }

    const char* input_path = argv[1];

    std::ifstream in(input_path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "Error: cannot open %s\n", input_path);
        return 1;
    }

    size_t file_size = static_cast<size_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> data(file_size);
    if (!in.read(reinterpret_cast<char*>(data.data()), file_size)) {
        std::fprintf(stderr, "Error: failed to read %s\n", input_path);
        return 1;
    }
    in.close();

    auto file = wk::parse_container(data);
    if (!file) {
        std::fprintf(stderr, "Error: %s\n", file.error().message.c_str());
        return 1;
    }

    if (!file->metadata) {
        std::fprintf(stderr, "No WKMETA chunk found in %s\n", input_path);
        return 0;
    }

    if (std::printf("%s", file->metadata->to_json().c_str()) < 0) {
        std::fprintf(stderr, "Error: failed to write output\n");
        return 1;
    }

    return 0;
}
