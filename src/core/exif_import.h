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


#include "common.h"
#include <wk/wkmeta.hpp>

namespace wk::meta {

std::expected<MetaBlock, MetaError> parse_exif_blob(std::span<const uint8_t> exif_blob);
std::expected<std::vector<uint8_t>, MetaError> extract_exif_blob_from_bytes(std::span<const uint8_t> file_bytes);

}
