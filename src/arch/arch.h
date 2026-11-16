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

// Compile-time target detection. Exactly one WK_ARCH_* macro is
// defined; WK_ARCH_GENERIC is the fallback for unknown targets, so a
// new target always builds through the generic path. arm64 and
// aarch64 are the same ISA — aarch64 is the canonical name here,
// arm64 is only an alias (see src/arch/README.md).
#if defined(__EMSCRIPTEN__)
#define WK_ARCH_WASM 1
#define WK_ARCH_NAME "wasm"
#elif defined(__x86_64__) || defined(_M_X64)
#define WK_ARCH_X86_64 1
#define WK_ARCH_NAME "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define WK_ARCH_AARCH64 1
#define WK_ARCH_NAME "aarch64"
#elif defined(__arm__) || defined(_M_ARM)
#define WK_ARCH_ARM 1
#define WK_ARCH_NAME "arm"
#else
#define WK_ARCH_GENERIC 1
#define WK_ARCH_NAME "generic"
#endif

namespace wk {

// Name of the target this library was compiled for. The dispatch in
// src/core/threading.h and the arch test both read this; keep it in
// sync with the macros above.
inline const char* arch_name() { return WK_ARCH_NAME; }

}  // namespace wk
