#include "services/chat_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <atomic>
#include <mutex>
#include <limits>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "inference/batch_scheduler.h"
#include "services/rag_prompt_formatter.h"

namespace fastllm {

namespace {

bool has_non_whitespace(const std::string& value) {
    for (unsigned char ch : value) {
        if (!std::isspace(ch)) return true;
    }
    return false;
}

}

struct ChatRequestState {
    std::atomic<bool> cancelled{false};
    std::string request_id;
    int user_id = 0;
    int tenant_id = 0;
};

struct ChatService::ActiveRequests {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<ChatRequestState>> sessions;
};

ChatService::ChatService(AppContext& ctx, ToolService& tools, AgentOptions options, bool allow_user_web_search)
    : active_(std::make_shared<ActiveRequests>()), ctx_(ctx), tools_(tools), options_(options),
      allow_user_web_search_(allow_user_web_search) {}

void ChatService::cancel(const HttpRequest& req, HttpResponse& response) {
    const auto authorization = req.header("Authorization");
    if (authorization.rfind("Bearer ", 0) != 0) { response.json_error(401, "Unauthorized"); return; }
    const auto claims = ctx_.jwt->verify_token(authorization.substr(7));
    if (!claims.valid) { response.json_error(401, "Unauthorized"); return; }
    try {
        const auto body = nlohmann::json::parse(req.body);
        const auto session = body.at("session_id").get<std::string>();
        const auto request = body.at("request_id").get<std::string>();
        std::lock_guard<std::mutex> lock(active_->mutex);
        auto item = active_->sessions.find(session);
        if (item == active_->sessions.end() || item->second->user_id != claims.user_id
            || item->second->tenant_id != claims.tenant_id || item->second->request_id != request) {
            response.json_error(404, "Active request not found"); return;
        }
        item->second->cancelled = true;
        response.json("{\"cancelled\":true}");
    } catch (const std::exception&) { response.json_error(400, "session_id and request_id required"); }
}

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

    const bool guest = claims.role == "guest";
    if (guest && (!ctx_.guest_enabled || claims.tenant_id != ctx_.guest_tenant_id
        || !ctx_.guest_limiter.allow_guest_chat(claims.user_id, req.peer_ip))) {
        send_sse_error(sse_ctx, "Guest chat limit reached or guest access disabled"); return;
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

        if (guest && message.size() > 2048) { send_sse_error(sse_ctx, "Guest message is too long"); return; }
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

        auto state = std::make_shared<ChatRequestState>();
        state->request_id = request_id;
        state->user_id = claims.user_id;
        state->tenant_id = claims.tenant_id;
        {
            std::lock_guard<std::mutex> lock(active_->mutex);
            if (active_->sessions.count(session_id)) {
                send_sse_error(sse_ctx, "This session already has an active request"); return;
            }
            active_->sessions[session_id] = state;
        }
        auto admission = std::shared_ptr<int>(new int(0), [active = active_, session_id, state](int* item) {
            {
                std::lock_guard<std::mutex> lock(active->mutex);
                auto entry = active->sessions.find(session_id);
                if (entry != active->sessions.end() && entry->second == state) active->sessions.erase(entry);
            }
            delete item;
        });

        std::vector<ChatMessage> history;
        for (const auto& stored : ctx_.session_repository->list_messages(session_id, 200)) {
            if ((stored.role == "user" || stored.role == "assistant")
                && !stored.content.empty()) {
                history.push_back({stored.role, stored.content, stored.reasoning});
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

        const bool use_agent = !guest && options_.enabled && engine->supports_tools();
        InferParams infer_params;
        infer_params.session_id = session_id;
        const int default_max_tokens = guest ? 512 : use_agent ? options_.max_output_tokens : (engine->supports_tools() ? 0 : 512);
        infer_params.max_tokens = body.value("max_tokens", default_max_tokens);
        if ((body.contains("max_tokens") && infer_params.max_tokens < 1)
            || infer_params.max_tokens < 0 || infer_params.max_tokens > 8192) {
            send_sse_error(sse_ctx, "max_tokens must be between 1 and 8192"); return;
        }
        if (guest) infer_params.max_tokens = std::min(infer_params.max_tokens, 512);
        infer_params.temperature = body.value("temperature", 0.7f);
        infer_params.top_p = body.value("top_p", 0.9f);
        infer_params.history = std::move(history);

        bool enable_rag = guest || body.value("enable_rag", use_agent);
        infer_params.system_prompt = guest ? "You are a research assistant for a public knowledge demo. Use only publicly available context and do not claim access to private laboratory data." : ctx_.system_prompt;
        nlohmann::json fallback_source_files = nlohmann::json::array();
        if (!use_agent && enable_rag && ctx_.knowledge_search_service) {
            SearchKnowledgeRequest search_request;
            search_request.query = message;
            search_request.top_k = 3;
            search_request.mode = KnowledgeSearchMode::CHUNKS;

            CallerContext caller;
            caller.public_only = guest;
            caller.tenant_id = claims.tenant_id;
            caller.principal_id = claims.user_id;
            caller.principal_type = "user";
            caller.trace_id = request_id;

            try {
                auto search_result = ctx_.knowledge_search_service->execute(
                    search_request, caller);
                infer_params.system_prompt = RagPromptFormatter::format(
                    search_result, infer_params.system_prompt);
                for (const auto& evidence : search_result.evidence) {
                    const bool seen = std::any_of(fallback_source_files.begin(), fallback_source_files.end(),
                        [&](const nlohmann::json& ref) { return ref["doc_id"] == evidence.document_id; });
                    if (seen) continue;
                    auto ref = tools_.download_reference(evidence.document_id, caller);
                    if (ref.is_object()) fallback_source_files.push_back(std::move(ref));
                }
            } catch (const KnowledgeSearchError& e) {
                spdlog::warn("RAG search skipped for chat request {}: {}", request_id, e.what());
            }
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
        infer_req->should_stop = [state, sse] { return state->cancelled.load() || !sse->is_active(); };
        infer_req->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options_.timeout_seconds);
        if (use_agent) {
            CallerContext caller;
            caller.tenant_id = claims.tenant_id;
            caller.principal_id = claims.user_id;
            caller.principal_type = "user";
            caller.trace_id = request_id;
            infer_req->owned_engine = std::make_shared<AgentRuntime>(*engine, tools_, *ctx_.rate_limiter,
                caller, options_, enable_rag, claims.role == "admin" || allow_user_web_search_,
                [sse](const nlohmann::json& event) {
                    if (sse->is_active()) sse->send_event("message", event.dump());
                }, [ctx, bill_tenant, bill_user]() -> long long {
                    if (!ctx->billing_service) return std::numeric_limits<long long>::max();
                    auto quota = ctx->billing_service->check_quota(bill_tenant, bill_user);
                    return quota.success ? quota.remaining : 0;
                });
            infer_req->engine = infer_req->owned_engine.get();
        }

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

        infer_req->on_done = [ctx, sse, utf8_buf, message, session_id, request_id, bill_tenant, bill_user, admission, active = active_, state, fallback_source_files](const InferResult& result) {
            const auto& source_files = result.source_files.empty() ? fallback_source_files : result.source_files;
            // Settle all completed model rounds, including cancelled/failed runs.
            int total_tokens = result.prompt_tokens + result.completion_tokens;
            BillingService::ChargeResult charge_result;
            if (ctx->billing_service && total_tokens > 0) {
                charge_result = ctx->billing_service->charge(
                    bill_tenant, bill_user, session_id, request_id,
                    result.prompt_tokens, result.completion_tokens);
            }
            bool messages_saved = false;
            if (!result.text.empty()) {
                messages_saved = ctx->message_repository && ctx->message_repository->insert_chat_pair(
                    session_id, message, result.text, result.prompt_tokens, result.completion_tokens, result.reasoning, source_files.dump());
            }
            {
                std::lock_guard<std::mutex> lock(active->mutex);
                auto entry = active->sessions.find(session_id);
                if (entry != active->sessions.end() && entry->second == state) active->sessions.erase(entry);
            }
            if (!result.error.empty()) {
                spdlog::error("Chat inference failed: session={} request={} error={}",
                              session_id, request_id, result.error);
                if (sse->is_active()) {
                    nlohmann::json err;
                    err["type"] = "error";
                    err["source_files"] = source_files;
                    err["error"] = result.error;
                    err["request_id"] = request_id;
                    err["stop_reason"] = result.stop_reason;
                    err["usage"] = {{"prompt_tokens", result.prompt_tokens}, {"completion_tokens", result.completion_tokens}, {"total_tokens", total_tokens}};
                    err["usage"]["estimated"] = result.usage_estimated;
                    err["messages"]["persisted"] = messages_saved;
                    err["billing"]["status"] = charge_result.record_status;
                    sse->send_event("error", err.dump());
                    sse->close();
                }
                return;
            }

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
            j["source_files"] = source_files;
            j["finish_reason"] = result.finish_reason;
            j["truncated"] = result.finish_reason == "length";
            j["request_id"] = request_id;
            j["usage"]["prompt_tokens"] = result.prompt_tokens;
            j["usage"]["completion_tokens"] = result.completion_tokens;
            j["usage"]["total_tokens"] = total_tokens;
            j["usage"]["estimated"] = result.usage_estimated;
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

        if (!ctx_.scheduler->submit(infer_req)) send_sse_error(sse, "Chat queue is full; retry later");
    } catch (const std::exception& e) {
        nlohmann::json err;
        err["error"] = std::string("Invalid JSON: ") + e.what();
        sse_ctx->send_event("error", err.dump());
        sse_ctx->close();
    }
}

}
