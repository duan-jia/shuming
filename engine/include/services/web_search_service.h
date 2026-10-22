#pragma once

#include <atomic>
#include <string>
#include <nlohmann/json.hpp>

namespace fastllm {

struct WebSearchOptions {
    bool enabled = false;
    std::string endpoint = "https://api.deepseek.com/v1/tools/search";
    std::string api_key;
    std::string ca_bundle = "/etc/ssl/certs/ca-certificates.crt";
    int timeout_seconds = 15;
    int max_concurrency = 2;
};

// Provider adapter; independent of HTTP routes and MCP framing.
class WebSearchService {
public:
    explicit WebSearchService(WebSearchOptions options);
    bool is_enabled() const;
    nlohmann::json execute(const nlohmann::json& arguments);
    static void validate(const nlohmann::json& arguments);
    static nlohmann::json input_schema();

private:
    WebSearchOptions options_;
    std::atomic<int> active_{0};
};

} // namespace fastllm
