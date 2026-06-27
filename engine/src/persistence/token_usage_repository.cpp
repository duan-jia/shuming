#include "persistence/token_usage_repository.h"

#include <mysql/mysql.h>

namespace fastllm {

std::string TokenUsageRepository::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

bool TokenUsageRepository::ensure_daily_usage(MysqlConnection& conn, int tenant_id, int user_id) {
    std::string sql =
        "INSERT INTO user_token_usage_daily "
        "(tenant_id, user_id, usage_date, token_limit, tokens_used) "
        "SELECT u.tenant_id, u.id, CURDATE(), t.token_limit_daily, 0 "
        "FROM users u JOIN tenants t ON t.id = u.tenant_id "
        "WHERE u.id=" + std::to_string(user_id)
        + " AND u.tenant_id=" + std::to_string(tenant_id)
        + " ON DUPLICATE KEY UPDATE tokens_used=tokens_used";
    return conn.execute(sql);
}

bool TokenUsageRepository::load_daily_usage(MysqlConnection& conn, int user_id, DailyTokenUsage& usage) {
    std::string sql =
        "SELECT token_limit, tokens_used, token_limit - tokens_used "
        "FROM user_token_usage_daily WHERE user_id=" + std::to_string(user_id)
        + " AND usage_date=CURDATE() LIMIT 1";
    auto* res = conn.query(sql);
    if (!res) return false;

    auto* row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return false;
    }

    usage.token_limit = row[0] ? std::stoll(row[0]) : 0;
    usage.tokens_used = row[1] ? std::stoll(row[1]) : 0;
    usage.remaining = row[2] ? std::stoll(row[2]) : 0;
    if (usage.remaining < 0) usage.remaining = 0;
    mysql_free_result(res);
    return true;
}

bool TokenUsageRepository::find_record_by_request_id(MysqlConnection& conn,
                                                     const std::string& request_id,
                                                     TokenUsageRecord& record) {
    std::string sql =
        "SELECT user_id, status, failure_reason FROM token_usage_records WHERE request_id='"
        + escape_sql(conn.raw(), request_id) + "' LIMIT 1";
    auto* res = conn.query(sql);
    if (!res) return false;

    auto* row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return false;
    }

    record.user_id = row[0] ? std::stoi(row[0]) : 0;
    record.status = row[1] ? row[1] : "pending";
    record.failure_reason = row[2] ? row[2] : "";
    mysql_free_result(res);
    return true;
}

bool TokenUsageRepository::insert_pending_record(MysqlConnection& conn, int tenant_id, int user_id,
                                                 const std::string& session_id,
                                                 const std::string& request_id,
                                                 int prompt_tokens, int completion_tokens) {
    int total_tokens = prompt_tokens + completion_tokens;
    std::string sql =
        "INSERT INTO token_usage_records "
        "(tenant_id, user_id, session_id, request_id, usage_date, prompt_tokens, "
        "completion_tokens, total_tokens, status) VALUES ("
        + std::to_string(tenant_id) + ","
        + std::to_string(user_id) + ",'"
        + escape_sql(conn.raw(), session_id) + "','"
        + escape_sql(conn.raw(), request_id) + "',CURDATE(),"
        + std::to_string(prompt_tokens) + ","
        + std::to_string(completion_tokens) + ","
        + std::to_string(total_tokens) + ",'pending')";
    return conn.execute(sql);
}

bool TokenUsageRepository::try_add_daily_usage(MysqlConnection& conn, int user_id, int total_tokens) {
    std::string sql =
        "UPDATE user_token_usage_daily SET tokens_used = tokens_used + "
        + std::to_string(total_tokens)
        + " WHERE user_id=" + std::to_string(user_id)
        + " AND usage_date=CURDATE()"
        + " AND tokens_used + " + std::to_string(total_tokens) + " <= token_limit";
    if (!conn.execute(sql)) {
        return false;
    }
    return mysql_affected_rows(conn.raw()) == 1;
}

bool TokenUsageRepository::update_record_status(MysqlConnection& conn,
                                                const std::string& request_id,
                                                const std::string& status,
                                                const std::string& failure_reason) {
    std::string sql = "UPDATE token_usage_records SET status='"
        + escape_sql(conn.raw(), status) + "', failure_reason=";
    if (failure_reason.empty()) {
        sql += "NULL";
    } else {
        sql += "'" + escape_sql(conn.raw(), failure_reason) + "'";
    }
    sql += " WHERE request_id='" + escape_sql(conn.raw(), request_id) + "'";
    return conn.execute(sql);
}

}
