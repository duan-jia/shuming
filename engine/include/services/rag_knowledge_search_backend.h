#pragma once

#include "rag/document_store.h"
#include "services/knowledge_search_service.h"

namespace fastllm {

class RagKnowledgeSearchBackend : public KnowledgeSearchBackend {
public:
    explicit RagKnowledgeSearchBackend(RagService& rag);

    bool is_ready() const override;
    QueryRewriteInfo rewrite_query(const std::string& query) const override;
    std::vector<Evidence> retrieve_chunks(
        const QueryRewriteInfo& query, int top_k, int tenant_id) override;
    std::vector<Evidence> retrieve_parents(
        const QueryRewriteInfo& query, int top_k, int tenant_id) override;

private:
    RagService& rag_;
};

} // namespace fastllm
