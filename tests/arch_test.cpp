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
