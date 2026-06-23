#pragma once

#include <faiss/IndexFlat.h>
#include <faiss/IndexIDMap.h>
#include <faiss/impl/IDSelector.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "rag/chunker.h"

namespace fastllm {

struct VectorEntry {
    int doc_id;
    int chunk_index;
    int tenant_id;
    std::string visibility;
    std::string content;
    ChunkMetadata metadata;
};

struct SearchResult {
    int doc_id;
    int chunk_index;
    float score;
    std::string content;
    ChunkMetadata metadata;
};

class VectorIndex {
public:
    VectorIndex();
    ~VectorIndex();

    faiss::idx_t add(const std::vector<float>& embedding, const VectorEntry& entry, int64_t chunk_id);
    void remove_by_doc(int doc_id);
    std::vector<SearchResult> search(const std::vector<float>& query,
                                      int top_k = 5,
                                      int tenant_id = -1) const;

    size_t size() const;
    size_t dim() const { return dim_; }

    bool save(const std::string& path) const;
    bool load(const std::string& path);

    const std::unordered_map<faiss::idx_t, VectorEntry>& entries() const { return entries_; }
    void restore_entry(faiss::idx_t id, const VectorEntry& entry);
    void set_next_id(faiss::idx_t id) { next_id_ = id; }
    faiss::idx_t next_id() const { return next_id_; }

private:
    void ensure_index(size_t dim);

    faiss::IndexIDMap* index_ = nullptr;
    size_t dim_ = 0;
    faiss::idx_t next_id_ = 1;

    std::unordered_map<faiss::idx_t, VectorEntry> entries_;
    std::unordered_map<int, std::vector<faiss::idx_t>> doc_to_ids_;

    mutable std::mutex mutex_;
};

}
