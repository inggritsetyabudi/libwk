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

// Only rans.cpp uses this (the explicit instantiations below), so
// it lives here instead of in rans.h.
template<int PrecisionBits = RANS_PRECISION_BITS>
class ContextRansTables {
public:
    explicit ContextRansTables(int num_contexts, int num_symbols)
        : num_contexts_(num_contexts), num_symbols_(num_symbols) {
        tables_.resize(num_contexts_);
    }

    void build_context(int context, const uint32_t* counts) {
        tables_[context].build_from_counts(counts, num_symbols_);
    }

    void build_all_uniform() {
        for (auto& table : tables_) {
            table.build_uniform(num_symbols_);
        }
    }

    [[nodiscard]] const RansTable<PrecisionBits>& table(int context) const {
        return tables_[context % num_contexts_];
    }

    [[nodiscard]] int num_contexts() const { return num_contexts_; }

private:
    int num_contexts_;
    int num_symbols_;
    std::vector<RansTable<PrecisionBits>> tables_;
};



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

template class ContextRansTables<10>;
template class ContextRansTables<11>;
template class ContextRansTables<12>;
template class ContextRansTables<14>;

}
