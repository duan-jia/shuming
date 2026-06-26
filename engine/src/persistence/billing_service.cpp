#include "persistence/billing_service.h"

#include "persistence/mysql_transaction.h"

#include <mysql/mysql.h>
#include <spdlog/spdlog.h>

namespace fastllm {

BillingService::BillingService(MysqlPool& mysql) : mysql_(mysql) {}

std::string BillingService::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

bool BillingService::load_daily_usage(MysqlConnection& conn, int user_id, ChargeResult& result) {
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

    result.token_limit = row[0] ? std::stoll(row[0]) : 0;
    result.tokens_used = row[1] ? std::stoll(row[1]) : 0;
    result.remaining = row[2] ? std::stoll(row[2]) : 0;
    if (result.remaining < 0) result.remaining = 0;
    mysql_free_result(res);
    return true;
}

bool BillingService::ensure_daily_usage(MysqlConnection& conn, int tenant_id, int user_id) {
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

bool BillingService::load_existing(MysqlConnection& conn, const std::string& request_id,
                                   ChargeResult& result) {
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

    int user_id = row[0] ? std::stoi(row[0]) : 0;
    result.duplicate = true;
    result.record_status = row[1] ? row[1] : "pending";
    result.failure_reason = row[2] ? row[2] : "";
    result.success = result.record_status == "success";
    mysql_free_result(res);

    if (user_id > 0) {
        load_daily_usage(conn, user_id, result);
    }
    return true;
}

BillingService::ChargeResult BillingService::check_quota(int tenant_id, int user_id) {
    ChargeResult result;

    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) {
        result.record_status = "failed";
        result.failure_reason = "mysql_unavailable";
        return result;
    }

    if (!ensure_daily_usage(*conn, tenant_id, user_id)
        || !load_daily_usage(*conn, user_id, result)) {
        result.record_status = "failed";
        result.failure_reason = "daily_usage_unavailable";
        mysql_.release(conn);
        return result;
    }

    result.success = result.remaining > 0;
    result.record_status = result.success ? "success" : "failed";
    if (!result.success) {
        result.failure_reason = "quota_exceeded";
    }
    mysql_.release(conn);
    return result;
}

BillingService::ChargeResult BillingService::charge(
    int tenant_id, int user_id, const std::string& session_id,
    const std::string& request_id, int prompt_tokens, int completion_tokens) {
    ChargeResult result;
    int total_tokens = prompt_tokens + completion_tokens;

    if (request_id.empty() || total_tokens <= 0) {
        result.record_status = "failed";
        result.failure_reason = "invalid_token_usage_request";
        return result;
    }

    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) {
        result.record_status = "failed";
        result.failure_reason = "mysql_unavailable";
        return result;
    }

    if (load_existing(*conn, request_id, result)) {
        mysql_.release(conn);
        return result;
    }

    std::string escaped_session = escape_sql(conn->raw(), session_id);
    std::string escaped_request = escape_sql(conn->raw(), request_id);

    MysqlTransaction tx(*conn);
    if (!tx.active()) {
        result.failure_reason = "transaction_start_failed";
        mysql_.release(conn);
        return result;
    }

    std::string insert_record =
        "INSERT INTO token_usage_records "
        "(tenant_id, user_id, session_id, request_id, usage_date, prompt_tokens, "
        "completion_tokens, total_tokens, status) VALUES ("
        + std::to_string(tenant_id) + ","
        + std::to_string(user_id) + ",'"
        + escaped_session + "','"
        + escaped_request + "',CURDATE(),"
        + std::to_string(prompt_tokens) + ","
        + std::to_string(completion_tokens) + ","
        + std::to_string(total_tokens) + ",'pending')";

    if (!conn->execute(insert_record)) {
        tx.rollback();
        if (load_existing(*conn, request_id, result)) {
            mysql_.release(conn);
            return result;
        }
        result.failure_reason = "usage_record_insert_failed";
        mysql_.release(conn);
        return result;
    }

    if (!ensure_daily_usage(*conn, tenant_id, user_id)) {
        tx.rollback();
        result.failure_reason = "daily_usage_init_failed";
        mysql_.release(conn);
        return result;
    }

    std::string update_daily =
        "UPDATE user_token_usage_daily SET tokens_used = tokens_used + "
        + std::to_string(total_tokens)
        + " WHERE user_id=" + std::to_string(user_id)
        + " AND usage_date=CURDATE()";

    if (!conn->execute(update_daily)) {
        tx.rollback();
        result.success = false;
        result.record_status = "failed";
        result.failure_reason = "daily_usage_update_failed";
        mysql_.release(conn);
        return result;
    }

    bool usage_updated = mysql_affected_rows(conn->raw()) == 1;

    load_daily_usage(*conn, user_id, result);

    if (usage_updated && result.tokens_used <= result.token_limit) {
        result.success = true;
        result.record_status = "success";
        result.failure_reason.clear();
    } else {
        result.success = false;
        result.record_status = "failed";
        result.failure_reason = "quota_exceeded";
    }

    std::string update_record =
        "UPDATE token_usage_records SET status='"
        + result.record_status + "', failure_reason=";
    if (result.failure_reason.empty()) {
        update_record += "NULL";
    } else {
        update_record += "'" + escape_sql(conn->raw(), result.failure_reason) + "'";
    }
    update_record += " WHERE request_id='" + escaped_request + "'";

    if (!conn->execute(update_record)) {
        tx.rollback();
        result.success = false;
        result.record_status = "failed";
        result.failure_reason = "usage_record_update_failed";
        mysql_.release(conn);
        return result;
    }

    if (!tx.commit()) {
        tx.rollback();
        result.success = false;
        result.record_status = "failed";
        result.failure_reason = "transaction_commit_failed";
        mysql_.release(conn);
        return result;
    }

    mysql_.release(conn);
    return result;
}

}
