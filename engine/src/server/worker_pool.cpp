#include "server/worker_pool.h"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace fastllm {

WorkerPool::WorkerPool(int threads, int queue_limit) : queue_limit_(queue_limit) {
    if (threads < 1 || queue_limit < 1) throw std::invalid_argument("Worker pool sizes must be positive");
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& thread : threads_) thread.join();
}

bool WorkerPool::submit(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || pending_.size() >= queue_limit_) return false;
        pending_.push(std::move(work));
    }
    ready_.notify_one();
    return true;
}

void WorkerPool::run() {
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) return;
            work = std::move(pending_.front());
            pending_.pop();
        }
        try { work(); }
        catch (const std::exception& error) { spdlog::error("Worker task failed: {}", error.what()); }
        catch (...) { spdlog::error("Worker task failed with an unknown exception"); }
    }
}

} // namespace fastllm
