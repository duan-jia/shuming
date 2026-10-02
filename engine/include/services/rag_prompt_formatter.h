#pragma once

#include <string>

#include "services/knowledge_search_types.h"

namespace fastllm {

class RagPromptFormatter {
public:
    static std::string format(
        const SearchKnowledgeResult& result,
        const std::string& fallback_system_prompt);
};

} // namespace fastllm
