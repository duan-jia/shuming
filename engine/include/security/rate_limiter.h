#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace fastllm {

class RateLimiter {
public:
    struct Rule {
        std::string action;
        int max_requests;
        int window_seconds;
    };

    RateLimiter();

    void add_rule(const std::string& action, int max_requests, int window_seconds);
    bool has_rule(const std::string& action) const;
    bool allow(int user_id, const std::string& action);

private:
    struct Counter {
        int count = 0;
        std::chrono::steady_clock::time_point window_start;
    };

    std::unordered_map<std::string, Rule> rules_;
    std::unordered_map<std::string, Counter> counters_;
    std::mutex mutex_;
};

}
