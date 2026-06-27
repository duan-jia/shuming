#include "persistence/billing_service.h"

#include "persistence/mysql_transaction.h"

namespace fastllm {

BillingService::BillingService(MysqlPool& mysql) : mysql_(mysql) {}

void BillingService::apply_daily_usage(const DailyTokenUsage& usage, ChargeResult& result) {
    result.token_limit = usage.token_limit;
    result.tokens_used = usage.tokens_used;
    result.remaining = usage.remaining;
    if (result.remaining < 0) result.remaining = 0;
}

bool BillingService::load_existing(MysqlConnection& conn, const std::string& request_id,
                                   ChargeResult& result) {
    TokenUsageRecord record;
    if (!token_usage_.find_record_by_request_id(conn, request_id, record)) {
        return false;
    }

    result.duplicate = true;
    result.record_status = record.status;
    result.failure_reason = record.failure_reason;
    result.success = result.record_status == "success";

    if (record.user_id > 0) {
        DailyTokenUsage usage;
        if (token_usage_.load_daily_usage(conn, record.user_id, usage)) {
            apply_daily_usage(usage, result);
        }
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

    DailyTokenUsage usage;
    if (!token_usage_.ensure_daily_usage(*conn, tenant_id, user_id)
        || !token_usage_.load_daily_usage(*conn, user_id, usage)) {
        result.record_status = "failed";
        result.failure_reason = "daily_usage_unavailable";
        mysql_.release(conn);
        return result;
    }

    apply_daily_usage(usage, result);
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

    MysqlTransaction tx(*conn);
    if (!tx.active()) {
        result.failure_reason = "transaction_start_failed";
        mysql_.release(conn);
        return result;
    }

    if (!token_usage_.insert_pending_record(*conn, tenant_id, user_id, session_id,
                                            request_id, prompt_tokens, completion_tokens)) {
        tx.rollback();
        if (load_existing(*conn, request_id, result)) {
            mysql_.release(conn);
            return result;
        }
        result.failure_reason = "usage_record_insert_failed";
        mysql_.release(conn);
        return result;
    }

    if (!token_usage_.ensure_daily_usage(*conn, tenant_id, user_id)) {
        tx.rollback();
        result.failure_reason = "daily_usage_init_failed";
        mysql_.release(conn);
        return result;
    }

    bool usage_updated = token_usage_.try_add_daily_usage(*conn, user_id, total_tokens);
    DailyTokenUsage updated_usage;
    if (token_usage_.load_daily_usage(*conn, user_id, updated_usage)) {
        apply_daily_usage(updated_usage, result);
    }

    if (usage_updated) {
        result.success = true;
        result.record_status = "success";
        result.failure_reason.clear();
    } else {
        result.success = false;
        result.record_status = "failed";
        result.failure_reason = "quota_exceeded";
    }

    if (!token_usage_.update_record_status(*conn, request_id, result.record_status,
                                           result.failure_reason)) {
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
