#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fastllm {

// Reject excess work instead of blocking a network event loop on a full queue.
class WorkerPool {
public:
    WorkerPool(int threads, int queue_limit);
    ~WorkerPool();
    bool submit(std::function<void()> work);
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

private:
    void run();
    const size_t queue_limit_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_ = false;
    std::queue<std::function<void()>> pending_;
    std::vector<std::thread> threads_;
};

} // namespace fastllm
