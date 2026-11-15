#pragma once
// ThreadPool.hpp —— 固定大小线程池，支持优雅停机（排空队列后再退出）。header-only。
#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstddef>

namespace oc {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t n) {
        if (n == 0) n = 1;
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            workers_.emplace_back([this] { worker(); });
    }
    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) return;
            tasks_.push(std::move(fn));
        }
        cv_.notify_one();
    }

    // 停止接收新任务，但把队列中已有任务执行完再退出。
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        return tasks_.size();
    }

private:
    void worker() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

} // namespace oc
