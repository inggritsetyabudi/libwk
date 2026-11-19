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

// 32-bit ARM CPU feature detection. Runtime detection only: NEON is
// optional on armv7. GCC/Clang use __builtin_cpu_supports (it reads
// HWCAP internally); MSVC has no runtime query and falls back to
// the compile-time __ARM_NEON macro.
namespace wk::arch {

#if defined(_MSC_VER)

inline CpuFeatures cpu_features() {
    CpuFeatures out;
#if defined(__ARM_NEON)
    out.neon = true;
#endif
    return out;
}

#else

inline CpuFeatures cpu_features() {
    CpuFeatures out;
#if defined(__clang__)
    // Clang has no __builtin_cpu_supports on 32-bit ARM: fall back
    // to the compile-time macro (set when the target enables NEON).
#if defined(__ARM_NEON)
    out.neon = true;
#endif
#else
    out.neon = __builtin_cpu_supports("neon") != 0;
#endif
    return out;
}

#endif

}  // namespace wk::arch
