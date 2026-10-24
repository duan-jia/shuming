#pragma once

#include <string>
#include <vector>

namespace fastllm {

enum class KnowledgeSearchMode {
    CHUNKS,
    PARENTS
};

struct SearchKnowledgeRequest {
    std::string query;
    int top_k = 5;
    KnowledgeSearchMode mode = KnowledgeSearchMode::CHUNKS;
};

struct CallerContext {
    int tenant_id = -1;
    int principal_id = 0;
    std::string principal_type;
    std::string trace_id;
    bool public_only = false;
};

struct QueryRewriteInfo {
    std::string original_query;
    std::string search_query;
    std::vector<std::string> aliases;
    bool rewritten = false;
    std::string failure_reason;

    std::string combined_query() const;
};

struct Evidence {
    KnowledgeSearchMode mode = KnowledgeSearchMode::CHUNKS;
    int document_id = 0;
    std::string filename;
    std::string content;
    float score = 0.0f;
    int chunk_index = -1;
    int matched_chunks = 0;
};

struct SearchKnowledgeResult {
    std::string query;
    QueryRewriteInfo rewrite;
    std::vector<Evidence> evidence;
};

} // namespace fastllm
