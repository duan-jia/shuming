#include "rag/query_rewriter.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace fastllm {

namespace {

bool contains_cjk(const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if ((c & 0xF0) == 0xE0 && i + 2 < value.size()) {
            unsigned char c1 = static_cast<unsigned char>(value[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(value[i + 2]);
            uint32_t code = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            if (code >= 0x4E00 && code <= 0x9FFF) return true;
            i += 2;
        }
    }
    return false;
}

void add_alias(std::vector<std::string>& aliases, const std::string& value) {
    if (value.empty()) return;
    auto same = [&value](const std::string& item) {
        return item == value;
    };
    if (std::find_if(aliases.begin(), aliases.end(), same) == aliases.end()) {
        aliases.push_back(value);
    }
}

std::string strip_json_fence(const std::string& text) {
    auto start = text.find('{');
    auto end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        return text;
    }
    return text.substr(start, end - start + 1);
}

} // namespace

std::string QueryRewriteResult::combined_query() const {
    std::string query = search_query.empty() ? original_query : search_query;
    if (!aliases.empty()) {
        query += "\nAliases:";
        for (const auto& alias : aliases) {
            query += " " + alias;
        }
    }
    if (!original_query.empty() && original_query != search_query) {
        query += "\nOriginal: " + original_query;
    }
    return query;
}

QueryRewriter::QueryRewriter(bool enabled,
                             std::string endpoint,
                             std::string model,
                             std::string api_key,
                             int timeout_seconds)
    : enabled_(enabled),
      endpoint_(std::move(endpoint)),
      model_(std::move(model)),
      api_key_(std::move(api_key)),
      timeout_seconds_(timeout_seconds) {}

QueryRewriteResult QueryRewriter::rewrite(const std::string& query) const {
    QueryRewriteResult result;
    result.original_query = query;
    result.search_query = query;

    if (!enabled_ || api_key_.empty() || endpoint_.empty() || query.empty()) {
        return result;
    }

    try {
        nlohmann::json body;
        body["model"] = model_.empty() ? "deepseek-chat" : model_;
        body["temperature"] = 0;
        body["response_format"] = {{"type", "json_object"}};
        body["messages"] = nlohmann::json::array({
            {
                {"role", "system"},
                {"content",
                 "Rewrite user search queries for an English document-card RAG system. "
                 "Return strict JSON only with keys search_query and aliases. "
                 "search_query must be concise English. aliases must preserve Chinese names, Chinese titles, "
                 "romanized names, acronyms, and exact technical terms from the original query. "
                 "Do not answer the query."}
            },
            {
                {"role", "user"},
                {"content", query}
            }
        });

        std::string url = endpoint_;
        if (!url.empty() && url.back() == '/') url.pop_back();
        bool use_https = url.rfind("https://", 0) == 0;
        std::string host = use_https ? url.substr(8) : url.substr(7);
        std::string path = "/chat/completions";
        size_t slash_pos = host.find('/');
        if (slash_pos != std::string::npos) {
            path = host.substr(slash_pos) + path;
            host = host.substr(0, slash_pos);
        }

        httplib::Headers headers = {
            {"Authorization", "Bearer " + api_key_},
            {"Content-Type", "application/json"},
        };

        std::unique_ptr<httplib::Response> response;
        if (use_https) {
            httplib::SSLClient cli(host);
            cli.enable_server_certificate_verification(false);
            cli.set_connection_timeout(timeout_seconds_, 0);
            cli.set_read_timeout(timeout_seconds_, 0);
            auto res = cli.Post(path, headers, body.dump(), "application/json");
            if (res) response = std::make_unique<httplib::Response>(*res);
        } else {
            httplib::Client cli(host);
            cli.set_connection_timeout(timeout_seconds_, 0);
            cli.set_read_timeout(timeout_seconds_, 0);
            auto res = cli.Post(path, headers, body.dump(), "application/json");
            if (res) response = std::make_unique<httplib::Response>(*res);
        }

        if (!response) {
            result.failure_reason = "http_error";
            return result;
        }
        if (response->status < 200 || response->status >= 300) {
            result.failure_reason = "http_" + std::to_string(response->status);
            return result;
        }

        auto outer = nlohmann::json::parse(response->body);
        std::string content = outer["choices"][0]["message"]["content"].get<std::string>();
        auto rewritten = nlohmann::json::parse(strip_json_fence(content));

        std::string search_query = rewritten.value("search_query", "");
        if (!search_query.empty()) {
            result.search_query = search_query;
            result.rewritten = true;
        }
        if (rewritten.contains("aliases") && rewritten["aliases"].is_array()) {
            for (const auto& item : rewritten["aliases"]) {
                if (item.is_string()) add_alias(result.aliases, item.get<std::string>());
            }
        }
        if (contains_cjk(query)) add_alias(result.aliases, query);
        return result;
    } catch (const std::exception& e) {
        result.failure_reason = e.what();
        spdlog::debug("Query rewrite failed: {}", e.what());
        return result;
    }
}

} // namespace fastllm
