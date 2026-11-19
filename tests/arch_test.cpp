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

#include <gtest/gtest.h>
#include "../src/arch/arch.h"
#include "../src/arch/cpu_features.h"

using namespace wk;

// The dispatch in src/arch/arch.h must resolve to exactly one target
// and report it. The #if chain here mirrors the header's own order,
// so a drift between the two (e.g. a new arch added to one but not
// the other) fails this test instead of silently picking generic.
TEST(ArchTest, NameMatchesCompileTimeTarget) {
    const char* name = arch_name();
    ASSERT_NE(name, nullptr);
    ASSERT_NE(name[0], '\0');
#if defined(__EMSCRIPTEN__)
    EXPECT_STREQ(name, "wasm");
#elif defined(__x86_64__) || defined(_M_X64)
    EXPECT_STREQ(name, "x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    EXPECT_STREQ(name, "aarch64");
#elif defined(__arm__) || defined(_M_ARM)
    EXPECT_STREQ(name, "arm");
#else
    EXPECT_STREQ(name, "generic");
#endif
}

TEST(ArchTest, ExactlyOneTargetMacroIsDefined) {
    int defined = 0;
#if defined(WK_ARCH_WASM)
    ++defined;
#endif
#if defined(WK_ARCH_X86_64)
    ++defined;
#endif
#if defined(WK_ARCH_AARCH64)
    ++defined;
#endif
#if defined(WK_ARCH_ARM)
    ++defined;
#endif
#if defined(WK_ARCH_GENERIC)
    ++defined;
#endif
    EXPECT_EQ(defined, 1);
}

// CPU feature detection (src/arch/cpu_features.h): the report is
// deterministic, and its flags must agree with the compile-time
// target. Only architecturally guaranteed flags are asserted
// positively; everything else is checked for consistency (a wider
// flag implies the narrower ones it builds on).
TEST(ArchTest, CpuFeaturesIsDeterministic) {
    const arch::CpuFeatures first = arch::cpu_features();
    const arch::CpuFeatures second = arch::cpu_features();
    EXPECT_EQ(first.sse2, second.sse2);
    EXPECT_EQ(first.sse4_1, second.sse4_1);
    EXPECT_EQ(first.avx2, second.avx2);
    EXPECT_EQ(first.avx512f, second.avx512f);
    EXPECT_EQ(first.neon, second.neon);
    EXPECT_EQ(first.sve, second.sve);
    EXPECT_EQ(first.sve2, second.sve2);
}

TEST(ArchTest, CpuFeaturesMatchesCompileTimeTarget) {
    const arch::CpuFeatures features = arch::cpu_features();
#if defined(__x86_64__) || defined(_M_X64)
    // SSE2 is part of the x86-64 baseline, so it must be present.
    EXPECT_TRUE(features.sse2);
    // AVX-512F implies AVX2, and AVX2 implies SSE4.1.
    if (features.avx512f) {
        EXPECT_TRUE(features.avx2);
    }
    if (features.avx2) {
        EXPECT_TRUE(features.sse4_1);
    }
    EXPECT_FALSE(features.neon);
    EXPECT_FALSE(features.sve);
    EXPECT_FALSE(features.sve2);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // NEON is mandatory on AArch64.
    EXPECT_TRUE(features.neon);
    // SVE2 implies SVE.
    if (features.sve2) {
        EXPECT_TRUE(features.sve);
    }
    EXPECT_FALSE(features.sse2);
    EXPECT_FALSE(features.sse4_1);
    EXPECT_FALSE(features.avx2);
    EXPECT_FALSE(features.avx512f);
#elif defined(__arm__) || defined(_M_ARM)
    // NEON is optional on 32-bit ARM: no positive assertion, only
    // that no x86 flag can ever be set here.
    EXPECT_FALSE(features.sse2);
    EXPECT_FALSE(features.sse4_1);
    EXPECT_FALSE(features.avx2);
    EXPECT_FALSE(features.avx512f);
    EXPECT_FALSE(features.sve);
    EXPECT_FALSE(features.sve2);
#else
    // Generic and wasm: detection reports nothing.
    EXPECT_FALSE(features.sse2);
    EXPECT_FALSE(features.sse4_1);
    EXPECT_FALSE(features.avx2);
    EXPECT_FALSE(features.avx512f);
    EXPECT_FALSE(features.neon);
    EXPECT_FALSE(features.sve);
    EXPECT_FALSE(features.sve2);
#endif
}
