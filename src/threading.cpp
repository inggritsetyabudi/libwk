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

#include "threading.h"

#include <system_error>

namespace wk {

namespace {
// Spawning threads is not free: an unbounded request (e.g. `--threads
// 100000`) used to try to create 100000 worker threads, hanging the
// encoder for over a minute. Cap the pool at a level any normal machine
// can actually run; more workers than this never help a tile-parallel
// encoder anyway.
constexpr uint32_t kMaxThreadPoolThreads = 256;
}  // namespace

ThreadPool::ThreadPool(uint32_t num_threads) {
    if (num_threads == 0) {
        num_threads = default_thread_count();
    }
    if (num_threads > kMaxThreadPoolThreads) {
        num_threads = kMaxThreadPoolThreads;
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        try {
            workers_.emplace_back([this](std::stop_token stoken) {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this, &stoken] {
                        return stop_ || !tasks_.empty() || stoken.stop_requested();
                    });

                    if ((stop_ || stoken.stop_requested()) && tasks_.empty()) {
                        return;
                    }

                    if (tasks_.empty()) continue;

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                task();
            }
            });
        } catch (const std::system_error&) {
            // The system cannot spawn another thread: keep the workers
            // created so far instead of failing the whole pool. With the
            // cap above this only triggers on a genuinely constrained
            // machine, not on a large request.
            break;
        }
    }

    if (workers_.empty()) {
        // A pool with no workers would queue tasks forever. This can only
        // happen when the system cannot spawn even one thread; surface it
        // as an exception the caller (encode) already converts to a
        // Result error instead of hanging.
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "ThreadPool could not spawn any worker thread");
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();

    for (auto& w : workers_) {
        w.request_stop();
        if (w.joinable()) w.join();
    }
}

uint32_t ThreadPool::default_thread_count() {
    auto n = std::thread::hardware_concurrency();
    return n > 0 ? n : 4;
}

}
