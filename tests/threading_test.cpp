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
#include <atomic>
#include <chrono>
#include "../src/threading.h"

using namespace wk;

TEST(ThreadingTest, HugeRequestIsCapped) {
    // Before the fix, ThreadPool(100000) tried to spawn 100000 threads:
    // `wkenc --threads 100000` hung for over a minute (killed by timeout)
    // instead of encoding. The pool must clamp to a sane maximum and
    // still work afterwards.
    ThreadPool pool(100000);
    EXPECT_GT(pool.num_threads(), 0u);
    EXPECT_LE(pool.num_threads(), 256u);

    auto future = pool.submit([] { return 42; });
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadingTest, ZeroMeansDefault) {
    ThreadPool pool(0);
    EXPECT_EQ(pool.num_threads(), ThreadPool::default_thread_count());
    EXPECT_GT(pool.num_threads(), 0u);
}

TEST(ThreadingTest, SmallRequestIsHonored) {
    ThreadPool pool(2);
    EXPECT_EQ(pool.num_threads(), 2u);

    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submit([&counter] { ++counter; }));
    }
    for (auto& f : futures) {
        f.get();
    }
    EXPECT_EQ(counter.load(), 8);
}

TEST(ThreadingTest, AtCapIsHonored) {
    ThreadPool pool(256);
    EXPECT_EQ(pool.num_threads(), 256u);
}
