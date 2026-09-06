#include "inference/batch_scheduler.h"
#include "inference/llm_engine.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace fastllm {

BatchScheduler::BatchScheduler() = default;

BatchScheduler::~BatchScheduler() {
    stop();
}

void BatchScheduler::start(int n_threads) {
    if (running_) return;
    running_ = true;
    n_threads_ = n_threads;

    for (int i = 0; i < n_threads; ++i) {
        workers_.emplace_back([this]() { scheduler_loop(); });
    }

    spdlog::info("BatchScheduler started with {} workers", n_threads);
}

void BatchScheduler::stop() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
    spdlog::info("BatchScheduler stopped");
}

void BatchScheduler::submit(std::shared_ptr<InferRequest> req) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push(req);
    }
    cv_.notify_one();
    spdlog::debug("Request submitted: session={}", req->session_id);
}

size_t BatchScheduler::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

size_t BatchScheduler::active_batch_size() const {
    return active_batch_.size();
}

void BatchScheduler::scheduler_loop() {
    while (running_) {
        std::shared_ptr<InferRequest> req;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !pending_.empty() || !running_; });
            if (!running_ && pending_.empty()) break;

            req = pick_next();
        }

        if (req) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_batch_.push_back(req);
            }

            process_request(req);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_batch_.erase(
                    std::remove(active_batch_.begin(), active_batch_.end(), req),
                    active_batch_.end());
            }
        }
    }
}

std::shared_ptr<InferRequest> BatchScheduler::pick_next() {
    if (pending_.empty()) return nullptr;

    auto req = pending_.front();
    pending_.pop();
    return req;
}

void BatchScheduler::process_request(std::shared_ptr<InferRequest> req) {
    if (!req->engine) {
        spdlog::error("BatchScheduler: request has no engine, cannot process");
        if (req->on_done) {
            InferResult empty;
            req->on_done(empty);
        }
        return;
    }

    spdlog::info("Processing request: session={}", req->session_id);

    InferParams params;
    params.session_id = req->session_id;
    params.system_prompt = req->system_prompt;
    params.history = req->history;
    params.max_tokens = req->max_tokens;
    params.temperature = req->temperature;
    params.top_p = req->top_p;
    params.top_k = req->top_k;

    req->engine->stream_complete(req->prompt, params,
        req->on_token,
        req->on_reasoning,
        req->on_done
    );
}

}
