#include "services/knowledge_search_service.h"

#include <cctype>

namespace fastllm {

namespace {

bool has_non_whitespace(const std::string& value) {
    for (unsigned char ch : value) {
        if (!std::isspace(ch)) return true;
    }
    return false;
}

} // namespace

std::string QueryRewriteInfo::combined_query() const {
    std::string combined = search_query.empty() ? original_query : search_query;
    if (!aliases.empty()) {
        combined += "\nAliases:";
        for (const auto& alias : aliases) {
            combined += " " + alias;
        }
    }
    if (!original_query.empty() && original_query != search_query) {
        combined += "\nOriginal: " + original_query;
    }
    return combined;
}

KnowledgeSearchError::KnowledgeSearchError(
    KnowledgeSearchErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

KnowledgeSearchService::KnowledgeSearchService(KnowledgeSearchBackend& backend)
    : backend_(backend) {}

SearchKnowledgeResult KnowledgeSearchService::execute(
    const SearchKnowledgeRequest& request,
    const CallerContext& caller) const {
    if (!has_non_whitespace(request.query)) {
        throw KnowledgeSearchError(
            KnowledgeSearchErrorCode::INVALID_ARGUMENT, "query required");
    }
    if (request.top_k < 1 || request.top_k > 100) {
        throw KnowledgeSearchError(
            KnowledgeSearchErrorCode::INVALID_ARGUMENT,
            "top_k must be between 1 and 100");
    }
    if (caller.tenant_id <= 0) {
        throw KnowledgeSearchError(
            KnowledgeSearchErrorCode::INVALID_ARGUMENT,
            "valid caller tenant required");
    }
    if (!backend_.is_ready()) {
        throw KnowledgeSearchError(
            KnowledgeSearchErrorCode::UNAVAILABLE,
            "RAG service not available");
    }

    SearchKnowledgeResult result;
    result.query = request.query;
    result.rewrite = backend_.rewrite_query(request.query);
    if (request.mode == KnowledgeSearchMode::PARENTS) {
        result.evidence = backend_.retrieve_parents(
            result.rewrite, request.top_k, caller.tenant_id);
    } else {
        result.evidence = backend_.retrieve_chunks(
            result.rewrite, request.top_k, caller.tenant_id);
    }
    return result;
}

KnowledgeSearchMode parse_knowledge_search_mode(const std::string& value) {
    if (value == "chunks") return KnowledgeSearchMode::CHUNKS;
    if (value == "parents") return KnowledgeSearchMode::PARENTS;
    throw KnowledgeSearchError(
        KnowledgeSearchErrorCode::INVALID_ARGUMENT,
        "mode must be 'chunks' or 'parents'");
}

} // namespace fastllm
