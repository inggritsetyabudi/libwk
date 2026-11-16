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

#include <cstdint>
#include <functional>
#include <future>

namespace wk {

// Wasm has no threads: tasks run synchronously on the submitting
// thread, in order. Same interface as the generic pool, so callers
// in src/core never branch on the target.
class ThreadPool {
public:
    explicit ThreadPool(std::uint32_t = 0) {}
    ~ThreadPool() = default;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<decltype(f(args...))> {
        using ReturnType = decltype(f(args...));
        std::packaged_task<ReturnType()> task(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        auto future = task.get_future();
        task();
        return future;
    }

    [[nodiscard]] std::uint32_t num_threads() const { return 1; }
    static std::uint32_t default_thread_count() { return 1; }
};

}  // namespace wk
