#pragma once
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fastllm {
// Process-local, bounded counters. Count attempts, not just failures, so concurrent
// requests cannot all pass before an earlier password verification completes.
class LoginLimiter {
public:
    using Clock = std::chrono::steady_clock;
    bool allow_source(const std::string& peer, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return take("global", 300, 60, now) && take("ip:" + peer, 60, 600, now);
    }
    bool allow_account(const std::string& account, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return take("account:" + account, 10, 600, now);
    }
private:
    struct Counter { Clock::time_point expires; int count = 0; };
    std::mutex mutex_;
    std::unordered_map<std::string, Counter> counters_;
    bool take(const std::string& key, int limit, int seconds, Clock::time_point now) {
        auto it = counters_.find(key);
        if (it == counters_.end()) {
            if (counters_.size() >= 8192) {
                for (auto item = counters_.begin(); item != counters_.end();) {
                    if (item->second.expires <= now) item = counters_.erase(item);
                    else ++item;
                }
                if (counters_.size() >= 8192) return false; // Fail closed, never evict live limits.
            }
            it = counters_.emplace(key, Counter{now + std::chrono::seconds(seconds), 0}).first;
        }
        if (it->second.expires <= now) it->second = {now + std::chrono::seconds(seconds), 0};
        if (it->second.count >= limit) return false;
        ++it->second.count;
        return true;
    }
};
}
