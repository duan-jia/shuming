#include "inference/remote_engine.h"

#include <algorithm>
#include <optional>
#include <vector>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

namespace fastllm {

RemoteEngine::RemoteEngine(const std::string& endpoint, const std::string& model,
                           const std::string& api_key, int context_window_tokens)
    : endpoint_(endpoint), model_(model), api_key_(api_key),
      context_window_tokens_(context_window_tokens > 0 ? context_window_tokens : 32768) {}

struct StreamCtx {
    std::function<void(const std::string&)> on_token;
    std::function<void(const std::string&)> on_reasoning;
    std::string full_text;
    std::string full_reasoning;
    std::string error;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    std::string line_buf;
};

struct ConversationTurn {
    ChatMessage user;
    ChatMessage assistant;
};

static size_t estimate_text_tokens(const std::string& text) {
    size_t ascii_count = 0;
    size_t non_ascii_count = 0;

    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0) {
            ++ascii_count;
            ++i;
            continue;
        }

        size_t codepoint_bytes = 1;
        if ((c & 0xE0) == 0xC0) codepoint_bytes = 2;
        else if ((c & 0xF0) == 0xE0) codepoint_bytes = 3;
        else if ((c & 0xF8) == 0xF0) codepoint_bytes = 4;
        i += std::min(codepoint_bytes, text.size() - i);
        ++non_ascii_count;
    }

    return (ascii_count + 3) / 4 + non_ascii_count * 2;
}

static size_t estimate_message_tokens(const ChatMessage& message) {
    return estimate_text_tokens(message.content) + 4;
}

static std::vector<ConversationTurn> collect_complete_turns(
    const std::vector<ChatMessage>& history) {
    std::vector<ConversationTurn> turns;
    std::optional<ChatMessage> pending_user;

    for (const auto& message : history) {
        if (message.role == "user") {
            pending_user = message;
        } else if (message.role == "assistant" && pending_user) {
            turns.push_back({*pending_user, message});
            pending_user.reset();
        }
    }
    return turns;
}

static bool build_messages(const std::string& prompt, const InferParams& params,
                           int context_window_tokens, nlohmann::json& messages,
                           std::string& error) {
    size_t safe_context = static_cast<size_t>(context_window_tokens) * 9 / 10;
    size_t output_reserve = params.max_tokens > 0
        ? static_cast<size_t>(params.max_tokens)
        : 0;
    if (output_reserve >= safe_context) {
        error = "max_tokens exceeds the configured remote context window";
        return false;
    }

    size_t input_budget = safe_context - output_reserve;
    size_t required_tokens = estimate_message_tokens({"user", prompt});
    if (!params.system_prompt.empty()) {
        required_tokens += estimate_message_tokens({"system", params.system_prompt});
    }
    if (required_tokens > input_budget) {
        error = "System prompt and current message exceed the configured remote context window";
        return false;
    }

    auto turns = collect_complete_turns(params.history);
    size_t first_selected = turns.size();
    size_t selected_tokens = 0;
    while (first_selected > 0) {
        const auto& turn = turns[first_selected - 1];
        size_t turn_tokens = estimate_message_tokens(turn.user)
            + estimate_message_tokens(turn.assistant);
        if (required_tokens + selected_tokens + turn_tokens > input_budget) break;
        selected_tokens += turn_tokens;
        --first_selected;
    }

    messages = nlohmann::json::array();
    if (!params.system_prompt.empty()) {
        messages.push_back({
            {"role", "system"},
            {"content", params.system_prompt}
        });
    }
    for (size_t i = first_selected; i < turns.size(); ++i) {
        messages.push_back({
            {"role", turns[i].user.role},
            {"content", turns[i].user.content}
        });
        messages.push_back({
            {"role", turns[i].assistant.role},
            {"content", turns[i].assistant.content}
        });
    }
    messages.push_back({
        {"role", "user"},
        {"content", prompt}
    });

    if (first_selected > 0) {
        spdlog::debug("RemoteEngine trimmed {} old conversation turn(s)", first_selected);
    }
    return true;
}

static bool parse_sse_line(const std::string& json_str, StreamCtx& ctx) {
    if (json_str == "[DONE]") return true;
    try {
        auto j = nlohmann::json::parse(json_str);
        if (j.contains("error")) {
            const auto& error = j["error"];
            if (error.is_object()) {
                ctx.error = error.value("message", error.dump());
            } else if (error.is_string()) {
                ctx.error = error.get<std::string>();
            } else {
                ctx.error = error.dump();
            }
            return true;
        }

        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& choices = j["choices"];
            auto& delta = choices[0]["delta"];

            if (delta.contains("content") && !delta["content"].is_null()) {
                auto content = delta["content"].get<std::string>();
                if (!content.empty()) {
                    ctx.full_text += content;
                    if (ctx.on_token) ctx.on_token(content);
                }
            }

            if (delta.contains("reasoning_content") &&
                !delta["reasoning_content"].is_null()) {
                auto reasoning = delta["reasoning_content"].get<std::string>();
                if (!reasoning.empty()) {
                    ctx.full_reasoning += reasoning;
                    if (ctx.on_reasoning) ctx.on_reasoning(reasoning);
                }
            }
        }

        if (j.contains("usage")) {
            auto& usage = j["usage"];
            if (usage.contains("prompt_tokens"))
                ctx.prompt_tokens = usage["prompt_tokens"].get<int>();
            if (usage.contains("completion_tokens"))
                ctx.completion_tokens = usage["completion_tokens"].get<int>();
        }
    } catch (const std::exception& e) {
        spdlog::debug("RemoteEngine SSE parse: {}", e.what());
    }
    return true;
}

static bool content_cb(const char* data, size_t len, uint64_t, uint64_t,
                       StreamCtx& ctx) {
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        ctx.line_buf += c;
        if (c != '\n') continue;

        std::string line = ctx.line_buf;
        ctx.line_buf.clear();

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("data: ", 0) == 0) {
            parse_sse_line(line.substr(6), ctx);
        } else if (line.front() == '{') {
            parse_sse_line(line, ctx);
        }
    }
    return true;
}

static void flush_line_buf(StreamCtx& ctx) {
    while (!ctx.line_buf.empty() &&
           (ctx.line_buf.back() == '\r' || ctx.line_buf.back() == '\n')) {
        ctx.line_buf.pop_back();
    }
    if (ctx.line_buf.empty()) return;
    if (ctx.line_buf.rfind("data: ", 0) == 0) {
        parse_sse_line(ctx.line_buf.substr(6), ctx);
    } else if (ctx.line_buf.front() == '{') {
        parse_sse_line(ctx.line_buf, ctx);
    }
    ctx.line_buf.clear();
}

void RemoteEngine::stream_complete(
    const std::string& prompt,
    const InferParams& params,
    std::function<void(const std::string&)> on_token,
    std::function<void(const std::string&)> on_reasoning,
    std::function<void(const InferResult&)> on_done) {

    StreamCtx ctx;
    ctx.on_token = std::move(on_token);
    ctx.on_reasoning = std::move(on_reasoning);

    nlohmann::json body;
    body["model"] = model_;
    body["stream"] = true;
    body["max_tokens"] = params.max_tokens;
    body["temperature"] = params.temperature;
    body["top_p"] = params.top_p;
    std::string build_error;
    if (!build_messages(prompt, params, context_window_tokens_, body["messages"], build_error)) {
        if (on_done) {
            InferResult result;
            result.error = std::move(build_error);
            on_done(result);
        }
        return;
    }

    std::string body_str = body.dump();

    std::string host_url = endpoint_;
    if (host_url.back() == '/') host_url.pop_back();

    bool use_https = (host_url.rfind("https://", 0) == 0);
    std::string host;
    if (use_https) {
        host = host_url.substr(8);
    } else {
        host = host_url.substr(7);
    }

    std::string path = "/chat/completions";
    size_t slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        path = host.substr(slash_pos) + path;
        host = host.substr(0, slash_pos);
    }

    for (int attempt = 1; attempt <= 2; ++attempt) {
        ctx.full_text.clear();
        ctx.full_reasoning.clear();
        ctx.error.clear();
        ctx.prompt_tokens = 0;
        ctx.completion_tokens = 0;
        ctx.line_buf.clear();

        httplib::Request req;
        req.method = "POST";
        req.path = path;
        req.headers = {
            {"Authorization", "Bearer " + api_key_},
            {"Content-Type", "application/json"},
            {"Accept", "text/event-stream"},
            {"Accept-Encoding", "identity"}
        };
        req.body = body_str;
        req.content_receiver = [&ctx](const char* data, size_t len,
                                      uint64_t off, uint64_t total) -> bool {
            return content_cb(data, len, off, total, ctx);
        };

        httplib::Result res;
        if (use_https) {
            httplib::SSLClient cli(host);
            cli.enable_server_certificate_verification(false);
            cli.set_read_timeout(300, 0);
            cli.set_connection_timeout(10, 0);
            res = cli.send(req);
        } else {
            httplib::Client cli(host);
            cli.set_read_timeout(300, 0);
            cli.set_connection_timeout(10, 0);
            res = cli.send(req);
        }

        bool retryable = false;
        if (!res) {
            ctx.error = "Remote model connection failed: " + httplib::to_string(res.error());
            retryable = true;
        } else {
            flush_line_buf(ctx);
            if (res->status < 200 || res->status >= 300) {
                if (ctx.error.empty()) {
                    ctx.error = "Remote model returned HTTP " + std::to_string(res->status);
                }
                retryable = res->status == 429 || res->status >= 500;
            } else if (ctx.error.empty() && ctx.full_text.empty() &&
                       ctx.full_reasoning.empty() && ctx.prompt_tokens == 0 &&
                       ctx.completion_tokens == 0) {
                ctx.error = "Remote model returned an empty response";
                retryable = true;
            }
        }

        if (ctx.error.empty()) break;
        if (attempt == 1 && retryable && ctx.full_text.empty() && ctx.full_reasoning.empty()) {
            spdlog::warn("RemoteEngine attempt {} failed: {}; retrying once", attempt, ctx.error);
            continue;
        }
        spdlog::error("RemoteEngine failed after {} attempt(s): {}", attempt, ctx.error);
        break;
    }

    if (on_done) {
        InferResult result;
        result.text = std::move(ctx.full_text);
        result.reasoning = std::move(ctx.full_reasoning);
        result.error = std::move(ctx.error);
        result.prompt_tokens = ctx.prompt_tokens;
        result.completion_tokens = ctx.completion_tokens;
        on_done(result);
    }
}

}
