#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace fastllm {

// cpp-httplib::Client::stop is thread-safe and interrupts a blocked read.
class RequestWatchdog {
public:
    RequestWatchdog(std::function<bool()> should_stop, std::function<void()> stop)
        : thread_([this, should_stop = std::move(should_stop), stop = std::move(stop)] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!done_) {
                if (ready_.wait_for(lock, std::chrono::milliseconds(20), [this] { return done_; })) break;
                lock.unlock();
                if (should_stop && should_stop()) stop();
                lock.lock();
            }
        }) {}
    ~RequestWatchdog() {
        { std::lock_guard<std::mutex> lock(mutex_); done_ = true; }
        ready_.notify_one();
        thread_.join();
    }
    RequestWatchdog(const RequestWatchdog&) = delete;
    RequestWatchdog& operator=(const RequestWatchdog&) = delete;
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    bool done_ = false;
    std::thread thread_;
};

} // namespace fastllm
