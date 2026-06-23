#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <mutex>

#include "rag/chunker.h"

namespace fastllm {

struct SearchResult; // fwd

struct Bm25Doc {
    int doc_id;
    int chunk_index;
    int tenant_id = 0;
    std::string visibility;
    ChunkMetadata metadata;
    std::string content;
    std::unordered_map<std::string, int> term_freq;
    int term_count = 0;
};

class Bm25Index {
public:
    void add(int doc_id, int chunk_index, const std::string& content,
             const ChunkMetadata& metadata, int tenant_id = 0,
             const std::string& visibility = "group");
    void remove_by_doc(int doc_id);
    std::vector<SearchResult> search(const std::string& query, int top_k,
                                      int tenant_id = -1) const;

    bool save(const std::string& path) const;
    bool load(const std::string& path);

    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return docs_.size(); }

private:
    std::vector<std::string> tokenize(const std::string& text) const;

    std::vector<Bm25Doc> docs_;
    std::unordered_map<std::string, std::unordered_set<int>> inv_; // term -> doc indices
    double k1_ = 1.5;
    double b_ = 0.75;
    mutable std::mutex mutex_;
};

}
