/**
 * @file Workers.h
 * @brief Fixed worker pool for handler work that blocks.
 *
 * Every store/DB call in this server is synchronous (execSqlSync, filesystem
 * reads and writes). Run on drogon's IO loops that is fatal: a loop thread
 * blocked in a query stops serving *every* connection the kernel later hands
 * it -- including /health -- so the server answers some requests in
 * milliseconds and silently never answers others.
 *
 * Handlers therefore hand their work to this pool and call the response
 * callback from a worker; drogon allows that from any thread (its own async
 * DB callbacks do the same), so no loop ever blocks.
 */

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace s3
{

class Workers
{
  public:
    /// @brief Start the pool once, from main().
    static void init(size_t threads)
    {
        auto& w = instance();
        for (size_t i = 0; i < threads; ++i)
            w.threads_.emplace_back([&w] { w.loop(); });
    }

    /// @brief Queue work. Runs on a worker thread, never on an IO loop.
    static void post(std::function<void()> job)
    {
        auto& w = instance();
        {
            std::lock_guard<std::mutex> lock(w.m_);
            w.jobs_.push(std::move(job));
        }
        w.cv_.notify_one();
    }

  private:
    static Workers& instance()
    {
        static Workers w;
        return w;
    }

    void loop()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            try {
                job();
            } catch (...) {
                // A handler that throws must not take the worker with it.
            }
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
};

} // namespace s3
