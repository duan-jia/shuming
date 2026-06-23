#include "security/rate_limiter.h"
#include "persistence/redis_manager.h"

#include <spdlog/spdlog.h>
#include <chrono>

namespace fastllm {

static const char* RATE_LIMIT_LUA = R"LUA(
local key    = KEYS[1]
local window = tonumber(ARGV[1])
local limit  = tonumber(ARGV[2])
local now    = tonumber(ARGV[3])
local req_id = ARGV[4]

redis.call('ZREMRANGEBYSCORE', key, 0, now - window * 1000)
local count = redis.call('ZCARD', key)
if count >= limit then
    return 0
end
redis.call('ZADD', key, now, req_id)
redis.call('EXPIRE', key, window + 1)
return 1
)LUA";

RateLimiter::RateLimiter() {}

void RateLimiter::add_rule(const std::string& action, int max_requests, int window_seconds) {
    rules_[action] = {action, max_requests, window_seconds};
}

bool RateLimiter::allow(int user_id, const std::string& action, RedisManager& redis) {
    auto it = rules_.find(action);
    if (it == rules_.end()) return true;

    auto& rule = it->second;
    std::string key = "rate:" + action + ":" + std::to_string(user_id);

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string req_uid = std::to_string(now_ms) + "_" + std::to_string(rand());

    long long result = 0;
    if (!redis.eval(RATE_LIMIT_LUA,
                    {key},
                    {std::to_string(rule.window_seconds),
                     std::to_string(rule.max_requests),
                     std::to_string(now_ms),
                     req_uid},
                    result)) {
        spdlog::warn("Rate limiter eval failed for user={} action={}", user_id, action);
        return true; // fail open
    }

    if (result == 0) {
        spdlog::info("Rate limit hit: user={} action={}", user_id, action);
    }
    return result > 0;
}

}