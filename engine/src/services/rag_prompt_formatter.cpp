#include "services/rag_prompt_formatter.h"

#include <iomanip>
#include <sstream>

namespace fastllm {

std::string RagPromptFormatter::format(
    const SearchKnowledgeResult& result,
    const std::string& fallback_system_prompt) {
    if (result.evidence.empty()) return fallback_system_prompt;

    std::ostringstream oss;
    oss << "Based on the following reference documents, answer the user's question.\n\n";
    oss << "=== Reference Documents ===\n\n";

    for (size_t i = 0; i < result.evidence.size(); ++i) {
        const auto& item = result.evidence[i];
        std::string filename = item.filename.empty()
            ? ("document #" + std::to_string(item.document_id))
            : item.filename;
        oss << "[" << (i + 1) << "] " << filename;
        if (item.mode == KnowledgeSearchMode::PARENTS) {
            oss << " (" << item.matched_chunks << " matching sections"
                << ", best score: " << std::fixed << std::setprecision(3)
                << item.score << ")\n";
        } else {
            oss << " (chunk " << item.chunk_index
                << ", relevance: " << std::fixed << std::setprecision(3)
                << item.score << ")\n";
        }
        oss << item.content << "\n\n";
    }

    oss << "=== User Question ===\n";
    oss << result.query << "\n\n";
    if (result.rewrite.rewritten
        && result.rewrite.search_query != result.query) {
        oss << "=== Search Query Used For Retrieval ===\n";
        oss << result.rewrite.combined_query() << "\n\n";
    }
    oss << "Please answer based on the reference documents above. "
        << "If the documents don't contain relevant information, say so.\n";
    return oss.str();
}

} // namespace fastllm
