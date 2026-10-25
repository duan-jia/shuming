#pragma once

#include <nlohmann/json.hpp>
#include "services/knowledge_search_service.h"
#include "services/web_search_service.h"
#include "services/knowledge_fetch_service.h"

namespace fastllm {

enum class ToolSet { ALL, KNOWLEDGE, WEB };

// Both the in-process runtime and the MCP adapter use this boundary.
class ToolService {
public:
    ToolService(KnowledgeSearchService& knowledge, WebSearchService& web, KnowledgeFetchService& documents);
    nlohmann::json list(ToolSet set, bool allow_web) const;
    ToolResult execute(const std::string& name, const nlohmann::json& arguments,
                           const CallerContext& caller, std::function<bool()> should_stop = nullptr);
    nlohmann::json download_reference(int id, const CallerContext& caller);
    static nlohmann::json knowledge_result(const SearchKnowledgeResult& result);

private:
    KnowledgeSearchService& knowledge_;
    WebSearchService& web_;
    KnowledgeFetchService& documents_;
};

} // namespace fastllm
