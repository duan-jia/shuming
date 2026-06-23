#pragma once

#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

namespace fastllm {

template<typename T>
class DoubleBuffer {
public:
    using BatchHandler = std::function<void(std::vector<T>&)>;

    DoubleBuffer(size_t flush_threshold = 100, BatchHandler handler = nullptr)
        : flush_threshold_(flush_threshold), handler_(handler) {}

    void push(const T& item) {
        std::lock_guard<std::recursive_mutex> lock(front_mutex_);
        front_buffer_.push_back(item);
        if (front_buffer_.size() >= flush_threshold_ && handler_) {
            swap_and_flush();
        }
    }

    void push(T&& item) {
        std::lock_guard<std::recursive_mutex> lock(front_mutex_);
        front_buffer_.push_back(std::move(item));
        if (front_buffer_.size() >= flush_threshold_ && handler_) {
            swap_and_flush();
        }
    }

    std::vector<T> swap_and_drain() {
        std::lock_guard<std::recursive_mutex> lock(front_mutex_);
        std::vector<T> result;
        result.swap(front_buffer_);
        return result;
    }

    void swap_and_flush() {
        auto batch = swap_and_drain();
        if (!batch.empty() && handler_) {
            handler_(batch);
        }
    }

    size_t front_size() const {
        std::lock_guard<std::recursive_mutex> lock(front_mutex_);
        return front_buffer_.size();
    }

    void set_handler(BatchHandler handler) { handler_ = std::move(handler); }
    void set_flush_threshold(size_t threshold) { flush_threshold_ = threshold; }

private:
    std::vector<T> front_buffer_;
    mutable std::recursive_mutex front_mutex_;
    size_t flush_threshold_;
    BatchHandler handler_;
};

}
