#pragma once

#include <string>
#include <unordered_map>

namespace fastllm {

class RedisManager;

class RateLimiter {
public:
    struct Rule {
        std::string action;
        int max_requests;
        int window_seconds;
    };

    RateLimiter();

    void add_rule(const std::string& action, int max_requests, int window_seconds);
    bool allow(int user_id, const std::string& action, RedisManager& redis);

private:
    std::unordered_map<std::string, Rule> rules_;
};

}
