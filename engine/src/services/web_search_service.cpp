#include "services/web_search_service.h"
#include "server/request_watchdog.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <httplib.h>
#include <regex>

namespace fastllm {
namespace {

void validate_query(const nlohmann::json& value, const char* name) {
    if (!value.is_string()) throw std::invalid_argument(std::string(name) + " must be a string");
    const auto text = value.get<std::string>();
    // JSON parsing has already validated UTF-8. Count code points, not bytes.
    size_t length = 0;
    for (unsigned char c : text) if ((c & 0xc0) != 0x80) ++length;
    if (length == 0 || length > 128
        || std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); })) {
        throw std::invalid_argument(std::string(name) + " must contain 1 to 128 characters");
    }
}

} // namespace

WebSearchService::WebSearchService(WebSearchOptions options) : options_(std::move(options)) {
    if (options_.timeout_seconds < 1 || options_.max_concurrency < 1) {
        throw std::invalid_argument("Invalid web search timeout or concurrency");
    }
}

bool WebSearchService::is_enabled() const { return options_.enabled; }

nlohmann::json WebSearchService::input_schema() {
    return {
        {"type", "object"}, {"additionalProperties", false}, {"required", {"query"}},
        {"properties", {
            {"query", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}, {"default", 5}}},
            {"need_content", {{"type", "boolean"}, {"default", false}}},
            {"ori_question", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}
        }}
    };
}

void WebSearchService::validate(const nlohmann::json& args) {
    if (!args.is_object() || !args.contains("query")) throw std::invalid_argument("query required");
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() != "query" && it.key() != "count" && it.key() != "need_content" && it.key() != "ori_question") {
            throw std::invalid_argument("Unknown web search argument: " + it.key());
        }
    }
    validate_query(args["query"], "query");
    if (args.contains("ori_question")) validate_query(args["ori_question"], "ori_question");
    if (args.contains("count") && (!args["count"].is_number_integer() || args["count"] < 1 || args["count"] > 50)) {
        throw std::invalid_argument("count must be an integer between 1 and 50");
    }
    if (args.contains("need_content") && !args["need_content"].is_boolean()) {
        throw std::invalid_argument("need_content must be a boolean");
    }
}

nlohmann::json WebSearchService::fetch_schema() {
    return {{"type", "object"}, {"additionalProperties", false}, {"required", {"url"}},
        {"properties", {{"url", {{"type", "string"}, {"minLength", 1}, {"maxLength", 8192},
            {"description", "Public HTTP(S) webpage URL. Never send private document text or credentials."}}}}}};
}

nlohmann::json WebSearchService::fetch(const nlohmann::json& args, std::function<bool()> should_stop) {
    if (!args.is_object() || args.size() != 1 || !args.contains("url") || !args["url"].is_string())
        throw std::invalid_argument("Only url is accepted");
    const auto url = args["url"].get<std::string>();
    // The provider fetches the target; the application never connects to arbitrary URLs.
    // Reject credentials and control characters before sharing the URL with the provider.
    static const std::regex pattern(R"(^https?://([^/?#@\s]+)([/?#].*)?$)");
    if (url.size() > 8192 || !std::regex_match(url, pattern)
        || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; }))
        throw std::invalid_argument("url must be an HTTP(S) URL with a host and no credentials or whitespace");
    const auto result = request(options_.fetch_endpoint, args, should_stop);
    if (!result.contains("title") || !result["title"].is_string()
        || !result.contains("content") || !result["content"].is_string())
        throw std::runtime_error("Web fetch upstream returned invalid content");
    return {{"url", url}, {"title", result["title"]}, {"content", result["content"]}};
}

nlohmann::json WebSearchService::request(const std::string& endpoint, const nlohmann::json& body,
                                       std::function<bool()> should_stop) {
    if (!options_.enabled || options_.api_key.empty()) throw std::runtime_error("Web search is not configured");
    const int previous = active_.fetch_add(1);
    struct Release { std::atomic<int>& active; ~Release() { --active; } } release{active_};
    if (previous >= options_.max_concurrency) throw std::runtime_error("Web search is busy; retry later");

    // Endpoint is operator configuration, never a tool argument. Plain HTTP is
    // restricted to loopback for deterministic local integration tests.
    const auto split = endpoint.find('/', endpoint.find("://") + 3);
    if (split == std::string::npos) throw std::runtime_error("Invalid web search endpoint");
    const auto origin = endpoint.substr(0, split);
    const auto path = endpoint.substr(split);
    if (origin.rfind("https://", 0) != 0 && origin.rfind("http://127.0.0.1:", 0) != 0) {
        throw std::runtime_error("Web search requires HTTPS");
    }
    httplib::Client client(origin);
    client.set_connection_timeout(options_.timeout_seconds, 0);
    client.set_read_timeout(options_.timeout_seconds, 0);
    client.set_write_timeout(options_.timeout_seconds, 0);
    client.set_follow_location(false);
    client.enable_server_certificate_verification(true);
    client.set_ca_cert_path(options_.ca_bundle.c_str());

    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = {{"Authorization", "Bearer " + options_.api_key}, {"Content-Type", "application/json"}};
    request.body = body.dump();
    std::string received;
    bool too_large = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options_.timeout_seconds);
    request.content_receiver = [&](const char* data, size_t length, uint64_t, uint64_t) {
        if (received.size() + length > 2 * 1024 * 1024) { too_large = true; return false; }
        if (std::chrono::steady_clock::now() > deadline) return false;
        received.append(data, length);
        return true;
    };
    if (should_stop && should_stop()) throw std::runtime_error("Request cancelled");
    httplib::Result response;
    {
        RequestWatchdog watchdog([&] { return (should_stop && should_stop()) || std::chrono::steady_clock::now() >= deadline; }, [&] { client.stop(); });
        response = client.send(request);
    }
    if (too_large) throw std::runtime_error("Web search response exceeds size limit");
    if (!response) throw std::runtime_error("Web search timed out or upstream is unreachable");
    if (response->status != 200) {
        // Do not forward upstream bodies: they may contain provider details or credentials.
        throw std::runtime_error("Web search upstream returned HTTP " + std::to_string(response->status));
    }
    auto result = nlohmann::json::parse(received, nullptr, false);
    if (!result.is_object()) throw std::runtime_error("Web provider returned invalid JSON");
    return result;
}

nlohmann::json WebSearchService::execute(const nlohmann::json& args, std::function<bool()> should_stop) {
    validate(args);
    auto body = args;
    if (!body.contains("count")) body["count"] = 5;
    if (!body.contains("need_content")) body["need_content"] = false;
    auto result = request(options_.endpoint, body, should_stop);
    if (!result.contains("results") || !result["results"].is_array()) {
        throw std::runtime_error("Web search upstream returned invalid results");
    }
    // Return only documented public evidence fields, never arbitrary upstream fields.
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : result["results"]) {
        if (!item.is_object() || !item.contains("title") || !item["title"].is_string()
            || !item.contains("url") || !item["url"].is_string()) {
            throw std::runtime_error("Web search upstream returned invalid evidence");
        }
        nlohmann::json evidence = nlohmann::json::object();
        for (const auto* key : {"title", "url", "snippet", "summary", "content", "date", "site_name", "rerank_score", "authority"}) {
            if (item.contains(key)) evidence[key] = item[key];
        }
        items.push_back(std::move(evidence));
        if (items.size() >= body["count"].get<size_t>()) break;
    }
    return {{"query", body["query"]}, {"results", std::move(items)}};
}

} // namespace fastllm
