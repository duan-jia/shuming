#pragma once

#include <algorithm>
#include <climits>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace fastllm {

// Collect only the successful, context-budgeted tool payload shown to the model.
// resolve rechecks current visibility and local PDF availability; never trust a URL.
inline void collect_source_files(nlohmann::json& refs, const std::string& tool,
                                 const std::string& content,
                                 const std::function<nlohmann::json(int)>& resolve) {
    using Json = nlohmann::json;
    if (tool != "search_knowledge" && tool != "fetch_knowledge") return;
    const auto delivered = Json::parse(content, nullptr, false);
    if (!delivered.is_object() || delivered.contains("error")) return;
    const auto candidates = tool == "search_knowledge"
        ? delivered.value("results", Json::array()) : Json::array({delivered});
    if (!candidates.is_array()) return;
    for (const auto& item : candidates) {
        if (refs.size() >= 40) break;
        if (!item.is_object() || !item.contains("doc_id") || !item["doc_id"].is_number_integer()
            || item["doc_id"] < 1 || item["doc_id"] > INT_MAX) continue;
        const int id = item["doc_id"].get<int>();
        if (std::any_of(refs.begin(), refs.end(), [id](const Json& ref) { return ref["doc_id"] == id; })) continue;
        auto ref = resolve(id);
        if (ref.is_object()) refs.push_back(std::move(ref));
    }
}

} // namespace fastllm
