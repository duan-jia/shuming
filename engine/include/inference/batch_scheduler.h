#pragma once

#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

#include "inference/i_engine.h"

namespace fastllm {

struct InferRequest {
    std::string session_id;
    std::string prompt;
    std::string system_prompt;
    std::vector<ChatMessage> history;
    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    int priority = 0;

    IInferenceEngine* engine = nullptr;
    std::shared_ptr<IInferenceEngine> owned_engine;
    std::function<bool()> should_stop;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

    std::function<void(const std::string&)> on_token;
    std::function<void(const std::string&)> on_reasoning;
    std::function<void(const InferResult&)> on_done;
};

class BatchScheduler {
public:
    BatchScheduler();
    ~BatchScheduler();

    void start(int n_threads = 1);
    void stop();

    bool submit(std::shared_ptr<InferRequest> req);

    size_t pending_count() const;
    size_t active_batch_size() const;
    bool is_running() const { return running_; }

    void set_max_batch_size(size_t size) { max_batch_size_ = size; }

private:
    void scheduler_loop();
    std::shared_ptr<InferRequest> pick_next();
    void process_request(std::shared_ptr<InferRequest> req);

    std::queue<std::shared_ptr<InferRequest>> pending_;
    std::vector<std::shared_ptr<InferRequest>> active_batch_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    size_t queue_limit_ = 64;
    size_t max_batch_size_ = 1;
    int n_threads_ = 1;
};

}
