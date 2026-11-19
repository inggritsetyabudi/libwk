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

// x86-64 CPU feature detection. Runtime detection only: the flags
// describe the machine this binary runs on, not the machine it was
// compiled for. GCC/Clang use __builtin_cpu_supports (it checks OS
// XSAVE support internally); MSVC uses __cpuid plus an XCR0 check.
namespace wk::arch {

#if defined(_MSC_VER)
#include <intrin.h>

inline CpuFeatures cpu_features() {
    CpuFeatures out;
    int info[4] = {};
    __cpuid(info, 0);
    const int max_leaf = info[0];
    if (max_leaf >= 1) {
        __cpuid(info, 1);
        out.sse2 = (info[3] & (1 << 26)) != 0;
        out.sse4_1 = (info[2] & (1 << 19)) != 0;
    }
    // AVX2/AVX512F need OS XSAVE support: XCR0 bits 1 (XMM) and 2
    // (YMM) set, plus bits 5-7 (ZMM state) for AVX-512.
    if (max_leaf >= 7) {
        __cpuidex(info, 7, 0);
        const bool avx2_cpu = (info[1] & (1 << 5)) != 0;
        const bool avx512f_cpu = (info[1] & (1 << 16)) != 0;
        const unsigned long long xcr0 = _xgetbv(0);
        const bool xmm_ok = (xcr0 & 0x2) != 0;
        const bool ymm_ok = (xcr0 & 0x4) != 0;
        const bool zmm_ok = (xcr0 & 0xE0) == 0xE0;
        out.avx2 = avx2_cpu && xmm_ok && ymm_ok;
        out.avx512f = avx512f_cpu && xmm_ok && ymm_ok && zmm_ok;
    }
    return out;
}

#else

inline CpuFeatures cpu_features() {
    CpuFeatures out;
    out.sse2 = __builtin_cpu_supports("sse2") != 0;
    out.sse4_1 = __builtin_cpu_supports("sse4.1") != 0;
    out.avx2 = __builtin_cpu_supports("avx2") != 0;
    out.avx512f = __builtin_cpu_supports("avx512f") != 0;
    return out;
}

#endif

}  // namespace wk::arch
