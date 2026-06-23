#include "inference/remote_engine.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <httplib.h>

namespace fastllm {

RemoteEngine::RemoteEngine(const std::string& endpoint, const std::string& model,
                           const std::string& api_key)
    : endpoint_(endpoint), model_(model), api_key_(api_key) {}

struct StreamCtx {
    std::function<void(const std::string&)> on_token;
    std::function<void(const std::string&)> on_reasoning;
    std::string full_text;
    std::string full_reasoning;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    std::string line_buf;
};

static bool parse_sse_line(const std::string& json_str, StreamCtx& ctx) {
    if (json_str == "[DONE]") return true;
    try {
        auto j = nlohmann::json::parse(json_str);
        auto& choices = j["choices"];
        if (!choices.empty()) {
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
        if (line.rfind("data: ", 0) != 0) continue;
        parse_sse_line(line.substr(6), ctx);
    }
    return true;
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
    body["messages"] = nlohmann::json::array();

    if (!params.system_prompt.empty()) {
        body["messages"].push_back({
            {"role", "system"},
            {"content", params.system_prompt}
        });
    }
    body["messages"].push_back({
        {"role", "user"},
        {"content", prompt}
    });

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

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.headers = {
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"}
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

    if (!res) {
        spdlog::error("RemoteEngine HTTP error: {}", httplib::to_string(res.error()));
    }

    if (on_done) {
        InferResult result;
        result.text = std::move(ctx.full_text);
        result.reasoning = std::move(ctx.full_reasoning);
        result.prompt_tokens = ctx.prompt_tokens;
        result.completion_tokens = ctx.completion_tokens;
        on_done(result);
    }
}

}
