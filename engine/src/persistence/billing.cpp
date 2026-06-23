#include "persistence/billing.h"
#include "persistence/redis_manager.h"

#include <spdlog/spdlog.h>
#include <ctime>
#include <sstream>

namespace fastllm {

static const char* BILLING_LUA = R"LUA(
local quota_key = KEYS[1]
local idemp_key = KEYS[2]
local tokens    = tonumber(ARGV[1])

if redis.call('EXISTS', idemp_key) == 1 then
    return -1
end

local remain = redis.call('DECRBY', quota_key, tokens)
if remain < 0 then
    redis.call('INCRBY', quota_key, tokens)
    return 0
end
redis.call('SETEX', idemp_key, 86400, '1')
return remain
)LUA";

std::string BillingManager::daily_key(int tenant_id) {
    auto t = std::time(nullptr);
    auto* tm = std::localtime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "quota:%d:%04d-%02d-%02d",
             tenant_id, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return buf;
}

BillingManager::DeductResult BillingManager::deduct(
    RedisManager& redis, int tenant_id, int user_id,
    int tokens, const std::string& request_id)
{
    std::string quota_key = daily_key(tenant_id);
    std::string idemp_key = "idempotent:" + request_id;

    long long result = 0;
    if (!redis.eval(BILLING_LUA,
                    {quota_key, idemp_key},
                    {std::to_string(tokens)},
                    result)) {
        spdlog::error("Billing eval failed: tenant={} req={}", tenant_id, request_id);
        return {OK, -1}; // fail open
    }

    DeductResult dr;
    if (result == -1) {
        dr.status = DUPLICATE;
        spdlog::debug("Billing: duplicate request {}", request_id);
    } else if (result == 0) {
        dr.status = INSUFFICIENT;
        dr.remaining = 0;
        spdlog::warn("Billing: insufficient quota tenant={}", tenant_id);
    } else {
        dr.status = OK;
        dr.remaining = result;
        spdlog::debug("Billing: deducted {} tokens tenant={} remaining={}", tokens, tenant_id, result);
    }

    return dr;
}

void BillingManager::set_daily_quota(RedisManager& redis, int tenant_id, int daily_limit) {
    redis.set(daily_key(tenant_id), std::to_string(daily_limit), 86400);
}

}