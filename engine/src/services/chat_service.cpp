#include "services/chat_service.h"

#include <cctype>
#include <cstdio>
#include <random>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "inference/batch_scheduler.h"

namespace fastllm {

namespace {

bool has_non_whitespace(const std::string& value) {
    for (unsigned char ch : value) {
        if (!std::isspace(ch)) return true;
    }
    return false;
}

}

ChatService::ChatService(AppContext& ctx) : ctx_(ctx) {}

std::string ChatService::generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t d[4] = {dist(gen), dist(gen), dist(gen), dist(gen)};
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
        d[0],
        (d[1] >> 16) & 0xFFFF,
        d[1] & 0x0FFF | 0x4000,
        (d[2] >> 16) & 0x3FFF | 0x8000,
        d[2] & 0xFFFF,
        d[3]);
    return std::string(buf);
}

void ChatService::send_sse_error(const std::shared_ptr<SseContext>& sse_ctx, const std::string& message) {
    nlohmann::json err;
    err["error"] = message;
    sse_ctx->send_event("error", err.dump());
    sse_ctx->close();
}

void ChatService::stream(const HttpRequest& req, const RouteParams& params, std::shared_ptr<SseContext> sse_ctx) {
    auto auth = req.header("Authorization", "");
    if (auth.substr(0, 7) != "Bearer ") {
        send_sse_error(sse_ctx, "Unauthorized");
        return;
    }

    auto claims = ctx_.jwt->verify_token(auth.substr(7));
    if (!claims.valid) {
        send_sse_error(sse_ctx, "Invalid token");
        return;
    }

    sse_ctx->user_id = claims.user_id;
    sse_ctx->tenant_id = claims.tenant_id;
    sse_ctx->role = claims.role;

    if (ctx_.rate_limiter && !ctx_.rate_limiter->allow(claims.user_id, "chat")) {
        send_sse_error(sse_ctx, "Rate limited");
        return;
    }

    try {
        auto body = nlohmann::json::parse(req.body);
        std::string message = body.value("message", "");
        std::string session_id = body.value("session_id", "");
        std::string model_name = body.value("model", "remote");
        std::string request_id = req.header("X-Request-Id", "");
        if (request_id.empty()) {
            request_id = body.value("request_id", "");
        }
        if (request_id.empty()) {
            request_id = generate_uuid();
        }

        if (!has_non_whitespace(message)) {
            send_sse_error(sse_ctx, "Message is required");
            return;
        }
        if (session_id.empty()) {
            send_sse_error(sse_ctx, "Session ID is required");
            return;
        }

        SessionSummary session;
        if (!ctx_.session_repository
            || !ctx_.session_repository->get_session(
                session_id, claims.tenant_id, claims.user_id, session)) {
            send_sse_error(sse_ctx, "Session not found");
            return;
        }

        std::vector<ChatMessage> history;
        for (const auto& stored : ctx_.session_repository->list_messages(session_id, 200)) {
            if ((stored.role == "user" || stored.role == "assistant")
                && !stored.content.empty()) {
                history.push_back({stored.role, stored.content});
            }
        }

        if (ctx_.billing_service) {
            auto quota = ctx_.billing_service->check_quota(claims.tenant_id, claims.user_id);
            if (!quota.success) {
                nlohmann::json err;
                err["error"] = quota.failure_reason.empty() ? "Quota unavailable" : quota.failure_reason;
                err["request_id"] = request_id;
                err["quota"]["status"] = quota.record_status;
                err["quota"]["token_limit"] = quota.token_limit;
                err["quota"]["tokens_used"] = quota.tokens_used;
                err["quota"]["remaining_tokens"] = quota.remaining;
                if (!quota.failure_reason.empty()) {
                    err["quota"]["failure_reason"] = quota.failure_reason;
                }
                sse_ctx->send_event("error", err.dump());
                sse_ctx->close();
                return;
            }
        }

        nlohmann::json started;
        started["type"] = "start";
        started["request_id"] = request_id;
        sse_ctx->send_event("message", started.dump());

        auto* engine = ctx_.model_mgr->get_engine(model_name);
        if (!engine || !engine->is_loaded()) {
            send_sse_error(sse_ctx, "No model loaded");
            return;
        }

        InferParams infer_params;
        infer_params.session_id = session_id;
        infer_params.max_tokens = body.value("max_tokens", 512);
        infer_params.temperature = body.value("temperature", 0.7f);
        infer_params.top_p = body.value("top_p", 0.9f);
        infer_params.history = std::move(history);

        bool enable_rag = body.value("enable_rag", false);
        if (enable_rag && ctx_.rag && ctx_.rag->is_ready()) {
            infer_params.system_prompt = ctx_.rag->build_rag_prompt(message, claims.tenant_id, 3);
        } else {
            infer_params.system_prompt = ctx_.system_prompt;
        }

        auto sse = sse_ctx;
        int bill_tenant = claims.tenant_id;
        int bill_user = claims.user_id;
        AppContext* ctx = &ctx_;

        auto infer_req = std::make_shared<InferRequest>();
        infer_req->session_id = session_id;
        infer_req->prompt = message;
        infer_req->system_prompt = infer_params.system_prompt;
        infer_req->history = std::move(infer_params.history);
        infer_req->max_tokens = infer_params.max_tokens;
        infer_req->temperature = infer_params.temperature;
        infer_req->top_p = infer_params.top_p;
        infer_req->top_k = infer_params.top_k;
        infer_req->engine = engine;

        auto utf8_buf = std::make_shared<std::string>();

        infer_req->on_token = [sse, utf8_buf](const std::string& token) {
            if (!sse->is_active()) return;
            *utf8_buf += token;
            std::string result;
            size_t total = utf8_buf->size();
            size_t i = 0;
            while (i < total) {
                unsigned char c = static_cast<unsigned char>((*utf8_buf)[i]);
                size_t len = 1;
                if ((c & 0x80) == 0)       len = 1;
                else if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                else { utf8_buf->erase(i, 1); total--; continue; }
                if (i + len > total) break;
                bool valid = true;
                for (size_t j = 1; j < len; j++) {
                    if ((static_cast<unsigned char>((*utf8_buf)[i+j]) & 0xC0) != 0x80)
                        { valid = false; break; }
                }
                if (!valid) { utf8_buf->erase(i, 1); total--; continue; }
                result += utf8_buf->substr(i, len);
                i += len;
            }
            utf8_buf->erase(0, i);
            if (!result.empty()) {
                nlohmann::json j;
                j["type"] = "answer";
                j["content"] = result;
                sse->send_event("message", j.dump());
            }
        };

        infer_req->on_reasoning = [sse](const std::string& token) {
            if (!sse->is_active()) return;
            nlohmann::json j;
            j["type"] = "reasoning";
            try {
                j["content"] = token;
                sse->send_event("message", j.dump());
            } catch (...) {}
        };

        infer_req->on_done = [ctx, sse, utf8_buf, message, session_id, request_id, bill_tenant, bill_user](const InferResult& result) {
            if (!result.error.empty()) {
                spdlog::error("Chat inference failed: session={} request={} error={}",
                              session_id, request_id, result.error);
                if (sse->is_active()) {
                    nlohmann::json err;
                    err["type"] = "error";
                    err["error"] = result.error;
                    err["request_id"] = request_id;
                    sse->send_event("error", err.dump());
                    sse->close();
                }
                return;
            }

            int total_tokens = result.prompt_tokens + result.completion_tokens;
            BillingService::ChargeResult charge_result;
            if (ctx->billing_service && total_tokens > 0) {
                charge_result = ctx->billing_service->charge(
                    bill_tenant, bill_user, session_id, request_id,
                    result.prompt_tokens, result.completion_tokens);
            }

            bool messages_saved = ctx->message_repository
                && ctx->message_repository->insert_chat_pair(
                    session_id, message, result.text,
                    result.prompt_tokens, result.completion_tokens);
            if (!messages_saved) {
                spdlog::error("Failed to persist chat messages synchronously: session={} request={}",
                              session_id, request_id);
            }

            if (!sse->is_active()) return;
            if (!utf8_buf->empty()) {
                nlohmann::json j;
                j["type"] = "answer";
                j["content"] = *utf8_buf;
                sse->send_event("message", j.dump());
                utf8_buf->clear();
            }

            nlohmann::json j;
            j["type"] = "done";
            j["request_id"] = request_id;
            j["usage"]["prompt_tokens"] = result.prompt_tokens;
            j["usage"]["completion_tokens"] = result.completion_tokens;
            j["usage"]["total_tokens"] = total_tokens;
            j["messages"]["persisted"] = messages_saved;
            if (!messages_saved) {
                j["messages"]["failure_reason"] = "message_insert_failed";
            }
            if (total_tokens > 0) {
                j["billing"]["status"] = charge_result.record_status;
                j["billing"]["remaining_tokens"] = charge_result.remaining;
                j["billing"]["duplicate"] = charge_result.duplicate;
                if (!charge_result.failure_reason.empty()) {
                    j["billing"]["failure_reason"] = charge_result.failure_reason;
                }
                j["quota"]["status"] = charge_result.record_status;
                j["quota"]["token_limit"] = charge_result.token_limit;
                j["quota"]["tokens_used"] = charge_result.tokens_used;
                j["quota"]["remaining_tokens"] = charge_result.remaining;
                j["quota"]["duplicate"] = charge_result.duplicate;
                if (!charge_result.failure_reason.empty()) {
                    j["quota"]["failure_reason"] = charge_result.failure_reason;
                }
            }
            sse->send_event("message", j.dump());
            sse->close();
        };

        ctx_.scheduler->submit(infer_req);
    } catch (const std::exception& e) {
        nlohmann::json err;
        err["error"] = std::string("Invalid JSON: ") + e.what();
        sse_ctx->send_event("error", err.dump());
        sse_ctx->close();
    }
}

}
