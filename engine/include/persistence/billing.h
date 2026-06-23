#pragma once

#include <string>

namespace fastllm {

class RedisManager;

class BillingManager {
public:
    static constexpr int OK         =  1;
    static constexpr int DUPLICATE  = -1;
    static constexpr int INSUFFICIENT = 0;

    static std::string daily_key(int tenant_id);

    struct DeductResult {
        int status = OK;
        long long remaining = 0;
    };

    static DeductResult deduct(RedisManager& redis, int tenant_id, int user_id,
                                int tokens, const std::string& request_id);

    static void set_daily_quota(RedisManager& redis, int tenant_id, int daily_limit);
};

}