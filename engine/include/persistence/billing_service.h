#pragma once

#include <string>

#include "persistence/mysql_pool.h"
#include "persistence/token_usage_repository.h"

namespace fastllm {

class BillingService {
public:
    struct ChargeResult {
        bool success = false;
        long long remaining = 0;
        long long token_limit = 0;
        long long tokens_used = 0;
        std::string record_status = "failed";
        std::string failure_reason;
        bool duplicate = false;
    };

    explicit BillingService(MysqlPool& mysql);

    ChargeResult check_quota(int tenant_id, int user_id);
    ChargeResult charge(int tenant_id, int user_id, const std::string& session_id,
                        const std::string& request_id, int prompt_tokens,
                        int completion_tokens);

private:
    MysqlPool& mysql_;
    TokenUsageRepository token_usage_;

    static void apply_daily_usage(const DailyTokenUsage& usage, ChargeResult& result);
    bool load_existing(MysqlConnection& conn, const std::string& request_id, ChargeResult& result);
};

}
