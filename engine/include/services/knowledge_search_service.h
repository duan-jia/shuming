#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "services/knowledge_search_types.h"

namespace fastllm {

enum class KnowledgeSearchErrorCode {
    INVALID_ARGUMENT,
    UNAVAILABLE
};

class KnowledgeSearchError : public std::runtime_error {
public:
    KnowledgeSearchError(KnowledgeSearchErrorCode code, const std::string& message);

    KnowledgeSearchErrorCode code() const { return code_; }

private:
    KnowledgeSearchErrorCode code_;
};

class KnowledgeSearchBackend {
public:
    virtual ~KnowledgeSearchBackend() = default;

    virtual bool is_ready() const = 0;
    virtual QueryRewriteInfo rewrite_query(const std::string& query) const = 0;
    virtual std::vector<Evidence> retrieve_chunks(
        const QueryRewriteInfo& query, int top_k, int tenant_id) = 0;
    virtual std::vector<Evidence> retrieve_parents(
        const QueryRewriteInfo& query, int top_k, int tenant_id) = 0;
};

class KnowledgeSearchService {
public:
    explicit KnowledgeSearchService(KnowledgeSearchBackend& backend);

    SearchKnowledgeResult execute(
        const SearchKnowledgeRequest& request,
        const CallerContext& caller) const;

private:
    KnowledgeSearchBackend& backend_;
};

KnowledgeSearchMode parse_knowledge_search_mode(const std::string& value);

} // namespace fastllm
