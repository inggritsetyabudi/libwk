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

// AArch64 CPU feature detection. Runtime detection only: the flags
// describe the machine this binary runs on. NEON is mandatory on
// AArch64; SVE/SVE2 are optional. GCC/Clang use
// __builtin_cpu_supports (it reads HWCAP internally); MSVC on ARM64
// has no runtime query, and NEON is required there by the ABI.
namespace wk::arch {

#if defined(_MSC_VER)

inline CpuFeatures cpu_features() {
    CpuFeatures out;
    out.neon = true;
    return out;
}

#else

inline CpuFeatures cpu_features() {
    CpuFeatures out;
#if defined(__clang__)
    // Clang spells Advanced SIMD "simd"; GCC spells it "neon".
    // (The builtin returns 0 for an unknown string, so the wrong
    // spelling would silently report no NEON.)
    out.neon = __builtin_cpu_supports("simd") != 0;
#else
    out.neon = __builtin_cpu_supports("neon") != 0;
#endif
    out.sve = __builtin_cpu_supports("sve") != 0;
    out.sve2 = __builtin_cpu_supports("sve2") != 0;
    return out;
}

#endif

}  // namespace wk::arch
