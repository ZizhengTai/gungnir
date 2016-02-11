/* Copyright 2015 Zizheng Tai
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GUNGNIR_HPP
#define GUNGNIR_HPP

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "gungnir/external/blockingconcurrentqueue.h"

namespace gungnir {

template <typename R>
using Task = std::function<R()>;

class TaskPool final {
public:
    explicit TaskPool(
            std::size_t numThreads = std::thread::hardware_concurrency())
        : numThreads_{numThreads}
    {
        threads_.reserve(numThreads_);

        for (std::size_t i = 0; i < numThreads_; ++i) {
            threads_.emplace_back([this] {
                moodycamel::ConsumerToken ctok{tasks_};
                Task<void> t;

                tasks_.wait_dequeue(ctok, t);
                while (t) {
                    t();
                    tasks_.wait_dequeue(ctok, t);
                }
            });
        }
    }

    ~TaskPool()
    {
        destroyed_ = true;  // prevent any future task dispatches

        for (std::size_t i = 0; i < numThreads_; ++i) {
            tasks_.enqueue(Task<void>{});
        }
        for (auto &t: threads_) {
            t.join();
        }

        // pump until empty
        std::atomic<std::size_t> numDones{0};
        for (auto &t: threads_) {
            t = std::thread([this, &numDones] {
                moodycamel::ConsumerToken ctok{tasks_};
                Task<void> t;

                do {
                    while (tasks_.try_dequeue(ctok, t)) {
                        t();
                    }
                } while (numThreads_ ==
                        numDones.fetch_add(1, std::memory_order_acq_rel) + 1);
            });
        }
        for (auto &t: threads_) {
            t.join();
        }
    }

    TaskPool(const TaskPool &other) = delete;
    TaskPool(TaskPool &&other) = delete;
    TaskPool & operator=(const TaskPool &other) = delete;
    TaskPool & operator=(TaskPool &&other) = delete;

    void dispatch(const Task<void> &task)
    {
        checkArgs(task);

        tasks_.enqueue(task);
    }

    template <typename R>
    std::future<R> dispatch(const Task<R> &task)
    {
        checkArgs(task);

        auto p = std::make_shared<std::promise<R>>();
        tasks_.enqueue([p, task]() mutable {
            try {
                p->set_value(task());
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        });
        return p->get_future();
    }

    template <typename Iter>
    void dispatch(Iter first, Iter last)
    {
        if (first >= last) {
            return;
        }
        checkArgs(first, last);

        tasks_.enqueue_bulk(first, last - first);
    }

    template <typename R, typename Iter>
    std::vector<std::future<R>> dispatch(Iter first, Iter last)
    {
        if (first >= last) {
            return {};
        }
        checkArgs(first, last);

        std::vector<std::future<R>> futures;
        futures.reserve(last - first);
        for (auto it = first; it != last; ++it) {
            futures.emplace_back(dispatch<R>(*it));
        }
        return futures;
    }

    template <typename Iter>
    void dispatchSync(Iter first, Iter last)
    {
        if (first >= last) {
            return;
        }
        checkArgs(first, last);

        std::atomic<std::size_t> count{static_cast<std::size_t>(last - first)};
        std::mutex m;
        std::condition_variable cv;

        for (auto it = first; it != last; ++it) {
            dispatch(std::bind([&](Task<void> t) {
                t();

                std::unique_lock<std::mutex> lk{m};
                if (--count == 0) {
                    cv.notify_all();
                }
            }, *it));
        }

        std::unique_lock<std::mutex> lk{m};
        cv.wait(lk, [&count] { return count == 0; });
    }

    template <typename R, typename Iter>
    std::vector<R> dispatchSync(Iter first, Iter last)
    {
        if (first >= last) {
            return {};
        }
        checkArgs(first, last);

        std::vector<std::future<R>> futures;
        futures.reserve(last - first);

        for (auto it = first; it != last; ++it) {
            futures.emplace_back(dispatch<R>(std::bind([&](decltype(*it) t) {
                return t();
            }, *it)));
        }

        std::vector<R> results;
        results.reserve(last - first);

        for (auto &f: futures) {
            results.emplace_back(f.get());
        }
        return results;
    }

    template <typename Iter>
    void dispatchSerial(Iter first, Iter last)
    {
        if (first >= last) {
            return;
        }
        checkArgs(first, last);

        auto tasks = std::make_shared<std::vector<Task<void>>>(first, last);
        dispatch([tasks] {
            for (const auto &t: *tasks) {
                t();
            }
        });
    }

    template <typename R, typename Iter>
    std::vector<std::future<R>> dispatchSerial(Iter first, Iter last)
    {
        if (first >= last) {
            return {};
        }
        checkArgs(first, last);

        auto promises =
            std::make_shared<std::vector<std::promise<R>>>(last - first);
        std::vector<std::future<R>> futures;
        futures.reserve(last - first);
        for (auto &p: *promises) {
            futures.emplace_back(p.get_future());
        }

        auto tasks = std::make_shared<std::vector<Task<R>>>(first, last);
        dispatch([tasks, promises] {
            for (std::size_t i = 0; i < tasks->size(); ++i) {
                try {
                    (*promises)[i].set_value((*tasks)[i]());
                } catch (...) {
                    (*promises)[i].set_exception(std::current_exception());
                }
            }
        });
        return futures;
    }

    void dispatchOnce(std::once_flag &flag, const Task<void> &task)
    {
        dispatch([task, &flag] {
            std::call_once(flag, task);
        });
    }

private:
    template <typename T>
    void checkArgs(const T &task) const
    {
        if (destroyed_) {
            throw std::runtime_error{"task pool already destroyed"};
        }
        if (!task) {
            throw std::invalid_argument{"task has no target callable object"};
        }
    }

    template <typename Iter>
    void checkArgs(Iter first, Iter last) const
    {
        using T = typename std::iterator_traits<Iter>::value_type;

        if (destroyed_) {
            throw std::runtime_error{"task pool already destroyed"};
        }
        if (!std::all_of(first, last, [](const T &t) { return t; })) {
            throw std::invalid_argument{"task has no target callable object"};
        }
    }

private:
    std::atomic<bool> destroyed_{false};
    const std::size_t numThreads_;
    std::vector<std::thread> threads_;
    moodycamel::BlockingConcurrentQueue<Task<void>> tasks_;
};

template <typename R, typename S>
void onSuccess(
        const std::shared_future<R> &future,
        const S &callback)
{
    std::thread{[=] {
        try {
            callback(future.get());
        } catch (...) {
        }
    }}.detach();
}

template <typename R, typename F>
void onFailure(
        const std::shared_future<R> &future,
        const F &callback)
{
    std::thread{[=] {
        try {
            future.get();
        } catch (...) {
            callback(std::current_exception());
        }
    }}.detach();
}

template <typename R, typename S, typename F>
void onComplete(
        const std::shared_future<R> &future,
        const S &success,
        const F &failure)
{
    std::thread{[=] {
        try {
            success(future.get());
        } catch (...) {
            failure(std::current_exception());
        }
    }}.detach();
}

}

#endif  // GUNGNIR_HPP
