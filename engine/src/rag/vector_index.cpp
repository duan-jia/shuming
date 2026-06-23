#include "rag/vector_index.h"

#include <faiss/index_io.h>
#include <spdlog/spdlog.h>

namespace fastllm {

VectorIndex::VectorIndex() = default;

VectorIndex::~VectorIndex() {
    if (index_) {
        delete index_;
        index_ = nullptr;
    }
}

void VectorIndex::ensure_index(size_t dim) {
    if (index_) return;
    auto* flat = new faiss::IndexFlatIP(static_cast<faiss::idx_t>(dim));
    index_ = new faiss::IndexIDMap(flat);
    index_->own_fields = true;
    dim_ = dim;
    spdlog::info("VectorIndex: FAISS IndexFlatIP(dim={}) created", dim);
}

faiss::idx_t VectorIndex::add(const std::vector<float>& embedding, const VectorEntry& entry, int64_t chunk_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    ensure_index(embedding.size());

    faiss::idx_t id = static_cast<faiss::idx_t>(chunk_id);
    index_->add_with_ids(1, embedding.data(), &id);
    entries_[id] = entry;
    doc_to_ids_[entry.doc_id].push_back(id);

    spdlog::debug("VectorIndex: added entry id={}, doc={}, chunk={}, total={}",
                  (long long)id, entry.doc_id, entry.chunk_index, entries_.size());
    return id;
}

void VectorIndex::remove_by_doc(int doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = doc_to_ids_.find(doc_id);
    if (it == doc_to_ids_.end() || it->second.empty()) return;

    faiss::IDSelectorBatch selector(it->second.size(), it->second.data());
    index_->remove_ids(selector);

    for (faiss::idx_t id : it->second) {
        entries_.erase(id);
    }
    doc_to_ids_.erase(it);

    spdlog::debug("VectorIndex: removed doc {}, remaining={}", doc_id, entries_.size());
}

std::vector<SearchResult> VectorIndex::search(const std::vector<float>& query,
                                                int top_k,
                                                int tenant_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!index_ || entries_.empty()) return {};

    int k = std::min(top_k * 3, static_cast<int>(index_->ntotal));
    if (k <= 0) return {};

    std::vector<faiss::idx_t> ids(k);
    std::vector<float> distances(k);

    index_->search(1, query.data(), k, distances.data(), ids.data());

    std::vector<SearchResult> results;
    for (int i = 0; i < k; ++i) {
        auto it = entries_.find(ids[i]);
        if (it == entries_.end()) continue;

        auto& e = it->second;
        if (e.visibility != "public" && tenant_id >= 0 && e.tenant_id != tenant_id) continue;

        SearchResult r;
        r.doc_id = e.doc_id;
        r.chunk_index = e.chunk_index;
        r.score = distances[i];
        r.content = e.content;
        r.metadata = e.metadata;
        results.push_back(r);

        if (static_cast<int>(results.size()) >= top_k) break;
    }

    return results;
}

size_t VectorIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool VectorIndex::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!index_) return false;
    try {
        faiss::write_index(index_, path.c_str());
        spdlog::info("VectorIndex: saved {} vectors to {}", entries_.size(), path);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("VectorIndex: save failed: {}", e.what());
        return false;
    }
}

bool VectorIndex::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        faiss::Index* raw = faiss::read_index(path.c_str());
        auto* idmap = dynamic_cast<faiss::IndexIDMap*>(raw);
        if (!idmap) {
            spdlog::error("VectorIndex: loaded index is not IndexIDMap");
            delete raw;
            return false;
        }
        if (index_) delete index_;
        index_ = idmap;
        dim_ = index_->d;
        spdlog::info("VectorIndex: loaded index from {}, dim={}, ntotal={}", path, dim_, index_->ntotal);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("VectorIndex: load failed: {}", e.what());
        return false;
    }
}

void VectorIndex::restore_entry(faiss::idx_t id, const VectorEntry& entry) {
    entries_[id] = entry;
    doc_to_ids_[entry.doc_id].push_back(id);
}

}
