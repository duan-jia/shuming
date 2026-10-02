#include "services/rag_knowledge_search_backend.h"

#include <utility>

namespace fastllm {

namespace {

QueryRewriteResult to_rag_query(const QueryRewriteInfo& query) {
    QueryRewriteResult result;
    result.original_query = query.original_query;
    result.search_query = query.search_query;
    result.aliases = query.aliases;
    result.rewritten = query.rewritten;
    result.failure_reason = query.failure_reason;
    return result;
}

} // namespace

RagKnowledgeSearchBackend::RagKnowledgeSearchBackend(RagService& rag) : rag_(rag) {}

bool RagKnowledgeSearchBackend::is_ready() const {
    return rag_.is_ready();
}

QueryRewriteInfo RagKnowledgeSearchBackend::rewrite_query(const std::string& query) const {
    auto rewritten = rag_.rewrite_query(query);
    QueryRewriteInfo result;
    result.original_query = rewritten.original_query;
    result.search_query = rewritten.search_query;
    result.aliases = std::move(rewritten.aliases);
    result.rewritten = rewritten.rewritten;
    result.failure_reason = std::move(rewritten.failure_reason);
    return result;
}

std::vector<Evidence> RagKnowledgeSearchBackend::retrieve_chunks(
    const QueryRewriteInfo& query, int top_k, int tenant_id) {
    auto matches = rag_.retrieve_prepared(to_rag_query(query), top_k, tenant_id);
    std::vector<Evidence> evidence;
    evidence.reserve(matches.size());
    for (auto& match : matches) {
        Evidence item;
        item.mode = KnowledgeSearchMode::CHUNKS;
        item.document_id = match.doc_id;
        item.filename = std::move(match.filename);
        item.content = std::move(match.content);
        item.score = match.score;
        item.chunk_index = match.chunk_index;
        evidence.push_back(std::move(item));
    }
    return evidence;
}

std::vector<Evidence> RagKnowledgeSearchBackend::retrieve_parents(
    const QueryRewriteInfo& query, int top_k, int tenant_id) {
    auto matches = rag_.retrieve_parents_prepared(to_rag_query(query), top_k, tenant_id);
    std::vector<Evidence> evidence;
    evidence.reserve(matches.size());
    for (auto& match : matches) {
        Evidence item;
        item.mode = KnowledgeSearchMode::PARENTS;
        item.document_id = match.doc_id;
        item.filename = std::move(match.filename);
        item.content = std::move(match.content);
        item.score = match.max_score;
        item.matched_chunks = match.matched_chunks;
        evidence.push_back(std::move(item));
    }
    return evidence;
}

} // namespace fastllm
