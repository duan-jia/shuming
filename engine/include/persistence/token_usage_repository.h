#pragma once

#include <string>

#include "persistence/mysql_pool.h"

namespace fastllm {

struct DailyTokenUsage {
    long long token_limit = 0;
    long long tokens_used = 0;
    long long remaining = 0;
};

struct TokenUsageRecord {
    int user_id = 0;
    std::string status = "pending";
    std::string failure_reason;
};

class TokenUsageRepository {
public:
    bool ensure_daily_usage(MysqlConnection& conn, int tenant_id, int user_id);
    bool load_daily_usage(MysqlConnection& conn, int user_id, DailyTokenUsage& usage);
    bool find_record_by_request_id(MysqlConnection& conn, const std::string& request_id,
                                   TokenUsageRecord& record);
    bool insert_pending_record(MysqlConnection& conn, int tenant_id, int user_id,
                               const std::string& session_id, const std::string& request_id,
                               int prompt_tokens, int completion_tokens);
    bool try_add_daily_usage(MysqlConnection& conn, int user_id, int total_tokens);
    bool update_record_status(MysqlConnection& conn, const std::string& request_id,
                              const std::string& status, const std::string& failure_reason);

private:
    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
