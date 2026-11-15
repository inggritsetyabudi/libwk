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

#include "rans.h"

namespace wk {

template class RansTable<10>;
template class RansTable<11>;
template class RansTable<12>;
template class RansTable<14>;

template class RansEncoder<10>;
template class RansEncoder<11>;
template class RansEncoder<12>;
template class RansEncoder<14>;

template class RansDecoder<10>;
template class RansDecoder<11>;
template class RansDecoder<12>;
template class RansDecoder<14>;

}
