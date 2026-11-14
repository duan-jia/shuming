#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace fastllm {
// Presentation only. This payload is NOT trusted/sanitized HTML; only the web
// renderer may interpret it, inside its sandbox and after allowlist filtering.
inline nlohmann::json html_display_schema() {
    return {{"name", "display_html"}, {"description",
        "Show a static card or table in the web chat. Supply an HTML fragment with inline styles, title and a plain-text summary. Maximum HTML size is 12000 UTF-8 bytes; at most four displays per request. Only basic text/table tags survive. No scripts, links, images, external resources, forms, buttons or callbacks. Always include the important answer in ordinary text too. This is presentation, not new evidence."},
        {"inputSchema", {{"type", "object"}, {"additionalProperties", false},
            {"required", {"title", "html", "summary"}}, {"properties", {
                {"title", {{"type", "string"}, {"minLength", 1}, {"maxLength", 120}}},
                {"html", {{"type", "string"}, {"minLength", 1}, {"maxLength", 12000}}},
                {"summary", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2000}}}
            }}}}};
}
inline nlohmann::json html_display(const nlohmann::json& args) {
    if (!args.is_object() || args.size() != 3) throw std::invalid_argument("Expected title, html and summary");
    for (const auto& field : {"title", "html", "summary"}) {
        const size_t limit = std::string(field) == "html" ? 12000 : std::string(field) == "title" ? 120 : 2000;
        if (!args.contains(field) || !args[field].is_string()) throw std::invalid_argument("Display fields must be strings");
        const auto text = args[field].get<std::string>();
        if (text.empty() || text.size() > limit || text.find_first_not_of(" \n\r\t") == std::string::npos)
            throw std::invalid_argument("Display field is empty or exceeds byte limit");
    }
    auto result = args;
    result["type"] = "html_display";
    return result;
}
} // namespace fastllm
