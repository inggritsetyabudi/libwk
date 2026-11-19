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

#include "arch.h"

// CPU feature detection, one interface for every target. Each target
// folder provides its own header with the same interface (see
// src/arch/<target>/cpu_features.h); this header only declares the
// result type and dispatches to the right one. Detection only — no
// SIMD kernel reads these flags yet (SIMD is Fase 12).
namespace wk::arch {

// Feature flags a target may report. Fields a target cannot have
// stay false; see the per-target headers for how each is detected.
struct CpuFeatures {
    bool sse2 = false;
    bool sse4_1 = false;
    bool avx2 = false;
    bool avx512f = false;
    bool neon = false;
    bool sve = false;
    bool sve2 = false;
};

// Detected per call; the result is deterministic for the lifetime
// of the process.
inline CpuFeatures cpu_features();

}  // namespace wk::arch

#if defined(WK_ARCH_X86_64)
#include "x86_64/cpu_features.h"
#elif defined(WK_ARCH_AARCH64)
#include "aarch64/cpu_features.h"
#elif defined(WK_ARCH_ARM)
#include "arm/cpu_features.h"
#elif defined(WK_ARCH_WASM)
#include "wasm/cpu_features.h"
#else
#include "generic/cpu_features.h"
#endif
