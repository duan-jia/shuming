#include "security/rate_limiter.h"

#include <spdlog/spdlog.h>

namespace fastllm {

RateLimiter::RateLimiter() {}

void RateLimiter::add_rule(const std::string& action, int max_requests, int window_seconds) {
    rules_[action] = {action, max_requests, window_seconds};
}

bool RateLimiter::has_rule(const std::string& action) const {
    return rules_.find(action) != rules_.end();
}

bool RateLimiter::allow(int user_id, const std::string& action) {
    auto it = rules_.find(action);
    if (it == rules_.end()) return true;

    auto& rule = it->second;
    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(rule.window_seconds);
    std::string key = action + ":" + std::to_string(user_id);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& counter = counters_[key];
    if (counter.window_start.time_since_epoch().count() == 0
        || now - counter.window_start >= window) {
        counter.window_start = now;
        counter.count = 0;
    }

    if (counter.count >= rule.max_requests) {
        spdlog::info("Rate limit hit: user={} action={}", user_id, action);
        return false;
    }

    counter.count++;
    return true;
}

}
