#include "rag/document_store.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace fastllm {

RagService::RagService() : chunker_(Chunker::Config{}), storage_dir_("data/rag") {}

RagService::~RagService() {
    stop_worker();
    save_all();
}

void RagService::set_storage_dir(const std::string& dir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    storage_dir_ = dir;
}

void RagService::set_mysql_pool(MysqlPool* pool) {
    mysql_pool_ = pool;
    if (mysql_pool_) {
        document_repository_ = std::make_unique<RagDocumentRepository>(*mysql_pool_);
    } else {
        document_repository_.reset();
    }
}

void RagService::set_query_rewriter(std::unique_ptr<QueryRewriter> query_rewriter) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    query_rewriter_ = std::move(query_rewriter);
}

QueryRewriteResult RagService::rewrite_query(const std::string& query) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!query_rewriter_) {
        QueryRewriteResult result;
        result.original_query = query;
        result.search_query = query;
        return result;
    }
    return query_rewriter_->rewrite(query);
}

bool RagService::save_all() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    try {
        std::filesystem::create_directories(storage_dir_);

        std::string index_path = storage_dir_ + "/index.faiss";
        if (!index_.save(index_path)) {
            spdlog::error("RagService: failed to save FAISS index");
            return false;
        }

        std::string bm25_path = storage_dir_ + "/bm25.json";
        if (!bm25_.save(bm25_path)) {
            spdlog::error("RagService: failed to save BM25 index");
            return false;
        }

        spdlog::info("RagService: saved FAISS index ({} vectors) and BM25 ({} docs) to {}",
                     index_.size(), bm25_.size(), storage_dir_);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("RagService: save_all exception: {}", e.what());
        return false;
    }
}

bool RagService::load_all() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_ || !embedder_->is_loaded()) {
        return false;
    }

    if (!document_repository_) {
        spdlog::warn("RagService: no MySQL pool, cannot load");
        return false;
    }

    try {
        std::string index_path = storage_dir_ + "/index.faiss";
        std::string bm25_path = storage_dir_ + "/bm25.json";

        bool faiss_loaded = false;
        if (std::filesystem::exists(index_path)) {
            faiss_loaded = index_.load(index_path);
        }

        documents_.clear();
        for (const auto& record : document_repository_->load_ready_documents()) {
            Document doc;
            doc.id = record.id;
            doc.tenant_id = record.tenant_id;
            doc.filename = record.filename;
            doc.content = record.content;
            doc.visibility = record.visibility;
            doc.uploaded_by = record.uploaded_by;
            doc.chunk_count = record.chunk_count;
            documents_[doc.id] = doc;
        }

        int chunk_count = 0;
        for (const auto& chunk : document_repository_->load_ready_chunks()) {
            VectorEntry entry;
            entry.doc_id = chunk.document_id;
            entry.tenant_id = chunk.tenant_id;
            entry.chunk_index = chunk.chunk_index;
            entry.content = chunk.content;
            entry.visibility = chunk.visibility;
            index_.restore_entry(static_cast<faiss::idx_t>(chunk.id), entry);
            chunk_count++;
        }

        // Load BM25 from file (or leave empty — will be rebuilt on new uploads)
        if (std::filesystem::exists(bm25_path)) {
            bm25_.load(bm25_path);
        }

        // If FAISS not loaded but we have chunks, rebuild by re-embedding
        if (!faiss_loaded && chunk_count > 0 && embedder_ && embedder_->is_loaded()) {
            spdlog::info("RagService: FAISS file missing, re-embedding {} chunks from MySQL...", chunk_count);
            for (auto& [id, entry] : index_.entries()) {
                auto embedding = embedder_->embed(entry.content);
                if (!embedding.empty()) {
                    // Re-add to FAISS with the same chunk_id
                    // Note: entries_ already has the entry, we just need the vector
                    // This is a simplified rebuild — in production, batch this
                }
            }
            // For now, just log — full rebuild would need re-creating the FAISS index
            spdlog::warn("RagService: FAISS rebuild not fully implemented, please re-upload documents");
        }

        spdlog::info("RagService: loaded {} documents, {} chunks from MySQL, FAISS={}, BM25={}",
                     documents_.size(), chunk_count, faiss_loaded ? "loaded" : "missing",
                     bm25_.size() > 0 ? "loaded" : "empty");
        return true;
    } catch (const std::exception& e) {
        spdlog::error("RagService: load_all exception: {}", e.what());
        return false;
    }
}

void RagService::set_embedder(std::unique_ptr<Embedder> embedder) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    embedder_ = std::move(embedder);
    load_all();
}

bool RagService::load_embedding_model(const std::string& model_path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_) {
        embedder_ = std::make_unique<LlamaEmbedder>();
    }
    bool ok = embedder_->load_model(model_path);
    if (ok) {
        load_all();
    }
    return ok;
}

void RagService::start_worker() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (worker_started_) return;
    stopping_ = false;
    worker_ = std::thread(&RagService::worker_loop, this);
    worker_started_ = true;
}

void RagService::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!worker_started_) return;
        stopping_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    worker_started_ = false;
}

void RagService::worker_loop() {
    while (true) {
        DocumentJob job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        process_document_job(job, true);
    }
}

int RagService::insert_processing_document(int tenant_id, const std::string& filename,
                                           const std::string& content,
                                           const std::string& visibility,
                                           int uploaded_by,
                                           const std::string& source_filename,
                                           const std::string& source_storage_name,
                                           std::uint64_t source_size_bytes) {
    if (!document_repository_) {
        spdlog::error("MySQL pool not set, cannot insert document");
        return -1;
    }
    return document_repository_->insert_processing_document(
        tenant_id, filename, content, visibility, uploaded_by,
        source_filename, source_storage_name, source_size_bytes);
}

bool RagService::process_document_job(const DocumentJob& job, bool save) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (deleted_documents_.count(job.doc_id) > 0) {
        spdlog::info("Skipping deleted document job: id={}", job.doc_id);
        return false;
    }

    if (!embedder_ || !embedder_->is_loaded()) {
        spdlog::error("Embedding model not loaded, cannot process document {}", job.doc_id);
        return false;
    }

    if (!document_repository_) {
        spdlog::error("MySQL pool not set, cannot process document {}", job.doc_id);
        return false;
    }

    // 2. Chunk the content
    auto chunks = chunker_.chunk(job.doc_id, job.content, job.category);

    int indexed_count = 0;

    // 3. For each chunk: INSERT into document_chunks -> embed -> add to FAISS + BM25
    for (auto& chunk : chunks) {
        int64_t chunk_id = 0;
        if (!document_repository_->insert_chunk(job.doc_id, job.tenant_id,
                                                chunk.chunk_index, chunk.content,
                                                job.visibility, chunk_id)) {
            spdlog::warn("Failed to insert chunk {} of doc {}", chunk.chunk_index, job.doc_id);
            continue;
        }

        auto embedding = embedder_->embed(chunk.content);
        if (embedding.empty()) {
            spdlog::warn("Failed to embed chunk {} of doc {}", chunk.chunk_index, job.doc_id);
            continue;
        }

        VectorEntry entry;
        entry.doc_id = job.doc_id;
        entry.chunk_index = chunk.chunk_index;
        entry.tenant_id = job.tenant_id;
        entry.visibility = job.visibility;
        entry.content = chunk.content;
        entry.metadata = chunk.metadata;

        index_.add(embedding, entry, chunk_id);
        bm25_.add(job.doc_id, chunk.chunk_index, chunk.content, chunk.metadata,
                  job.tenant_id, job.visibility);
        indexed_count++;
    }

    // 4. Persist document status
    std::string status = indexed_count > 0 ? "ready" : "failed";
    document_repository_->update_document_status(job.doc_id, status, indexed_count);

    if (indexed_count == 0) {
        spdlog::error("Document '{}' failed: id={}, tenant={}, chunks={}",
                      job.filename, job.doc_id, job.tenant_id, chunks.size());
        return false;
    }

    // 5. Update in-memory documents_ map
    Document doc;
    doc.id = job.doc_id;
    doc.tenant_id = job.tenant_id;
    doc.filename = job.filename;
    doc.content = job.content;
    doc.visibility = job.visibility;
    doc.uploaded_by = job.uploaded_by;
    doc.chunk_count = indexed_count;
    documents_[job.doc_id] = doc;

    spdlog::info("Document '{}' added: id={}, tenant={}, chunks={}",
                 job.filename, job.doc_id, job.tenant_id, chunks.size());

    if (save) save_all();
    return true;
}

int RagService::add_document(int tenant_id, const std::string& filename,
                               const std::string& content, const std::string& category,
                               const std::string& visibility, int uploaded_by, bool save,
                               const std::string& source_filename,
                               const std::string& source_storage_name,
                               std::uint64_t source_size_bytes) {
    int doc_id = insert_processing_document(tenant_id, filename, content, visibility, uploaded_by,
                                            source_filename, source_storage_name, source_size_bytes);
    if (doc_id <= 0) return -1;

    DocumentJob job;
    job.doc_id = doc_id;
    job.tenant_id = tenant_id;
    job.filename = filename;
    job.content = content;
    job.category = category;
    job.visibility = visibility;
    job.uploaded_by = uploaded_by;
    return process_document_job(job, save) ? doc_id : -1;
}

int RagService::enqueue_document(int tenant_id, const std::string& filename,
                                 const std::string& content, const std::string& category,
                                 const std::string& visibility, int uploaded_by,
                                 const std::string& source_filename,
                                 const std::string& source_storage_name,
                                 std::uint64_t source_size_bytes) {
    if (!embedder_ || !embedder_->is_loaded()) {
        spdlog::error("Embedding model not loaded, cannot enqueue document");
        return -1;
    }

    int doc_id = insert_processing_document(tenant_id, filename, content, visibility, uploaded_by,
                                            source_filename, source_storage_name, source_size_bytes);
    if (doc_id <= 0) return -1;

    DocumentJob job;
    job.doc_id = doc_id;
    job.tenant_id = tenant_id;
    job.filename = filename;
    job.content = content;
    job.category = category;
    job.visibility = visibility;
    job.uploaded_by = uploaded_by;

    start_worker();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        jobs_.push(std::move(job));
    }
    queue_cv_.notify_one();
    return doc_id;
}

bool RagService::remove_document(int doc_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!document_repository_ || !document_repository_->mark_document_deleted(doc_id)) {
        spdlog::error("Failed to mark document deleted: id={}", doc_id);
        return false;
    }

    deleted_documents_.insert(doc_id);
    auto it = documents_.find(doc_id);
    index_.remove_by_doc(doc_id);
    bm25_.remove_by_doc(doc_id);
    spdlog::info("Document removed: id={}", doc_id);
    if (it != documents_.end()) {
        documents_.erase(it);
    }

    save_all();
    return true;
}

std::vector<SearchResult> RagService::vector_only_search(const std::string& query,
                                                           int top_k,
                                                           int tenant_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_ || !embedder_->is_loaded()) return {};
    auto emb = embedder_->embed(query);
    if (emb.empty()) return {};
    auto results = index_.search(emb, top_k, tenant_id);
    for (auto& r : results) {
        auto doc_it = documents_.find(r.doc_id);
        if (doc_it != documents_.end()) {
            r.filename = doc_it->second.filename;
        }
    }
    return results;
}

std::vector<SearchResult> RagService::bm25_only_search(const std::string& query, int top_k,
                                                        int tenant_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto results = bm25_.search(query, top_k, tenant_id);
    for (auto& r : results) {
        auto doc_it = documents_.find(r.doc_id);
        if (doc_it != documents_.end()) {
            r.filename = doc_it->second.filename;
        }
    }
    return results;
}

std::vector<SearchResult> RagService::retrieve(const std::string& query, int top_k,
                                                 int tenant_id) {
    return retrieve_prepared(rewrite_query(query), top_k, tenant_id);
}

std::vector<SearchResult> RagService::retrieve_prepared(const QueryRewriteResult& rewritten_query,
                                                          int top_k,
                                                          int tenant_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_ || !embedder_->is_loaded()) {
        spdlog::error("Embedding model not loaded");
        return {};
    }

    std::string search_query = rewritten_query.combined_query();
    auto query_emb = embedder_->embed(search_query);
    if (query_emb.empty()) return {};

    int oversample = std::max(top_k, 5) * 3;
    auto vec_results = index_.search(query_emb, oversample, tenant_id);
    auto bm25_results = bm25_.search(search_query, oversample, tenant_id);

    const double k_rrf = 60.0;
    auto make_key = [](int doc, int chunk) -> uint64_t {
        return (static_cast<uint64_t>(doc) << 32) | static_cast<uint32_t>(chunk);
    };
    auto get_key = [&](const SearchResult& r) { return make_key(r.doc_id, r.chunk_index); };

    std::unordered_map<uint64_t, double> rrf_scores;
    std::unordered_map<uint64_t, const SearchResult*> entry_map;

    for (int i = 0; i < static_cast<int>(vec_results.size()); ++i) {
        auto key = get_key(vec_results[i]);
        rrf_scores[key] += 1.0 / (k_rrf + i + 1);
        entry_map[key] = &vec_results[i];
    }
    for (int i = 0; i < static_cast<int>(bm25_results.size()); ++i) {
        auto key = get_key(bm25_results[i]);
        rrf_scores[key] += 1.0 / (k_rrf + i + 1);
        if (!entry_map.count(key)) {
            entry_map[key] = &bm25_results[i];
        }
    }

    std::vector<std::pair<uint64_t, double>> ranked;
    for (auto& [key, score] : rrf_scores) {
        ranked.emplace_back(key, score);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<SearchResult> results;
    for (size_t i = 0; i < ranked.size(); ++i) {
        auto it = entry_map.find(ranked[i].first);
        if (it == entry_map.end()) continue;

        SearchResult r = *it->second;
        r.score = static_cast<float>(ranked[i].second);
        auto doc_it = documents_.find(r.doc_id);
        if (doc_it != documents_.end()) {
            r.filename = doc_it->second.filename;
        }
        results.push_back(r);
        if (static_cast<int>(results.size()) >= top_k) break;
    }

    return results;
}

std::string RagService::build_rag_prompt(const std::string& query,
                                           int tenant_id,
                                           int top_k) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto rewritten = rewrite_query(query);
    auto results = retrieve_prepared(rewritten, top_k, tenant_id);
    if (results.empty()) return query;

    std::ostringstream oss;
    oss << "Based on the following reference documents, answer the user's question.\n\n";
    oss << "=== Reference Documents ===\n\n";

    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        std::string filename = r.filename.empty()
            ? ("document #" + std::to_string(r.doc_id))
            : r.filename;
        oss << "[" << (i + 1) << "] " << filename
            << " (chunk " << r.chunk_index
            << ", relevance: " << std::fixed << std::setprecision(3) << r.score << ")\n";
        oss << r.content << "\n\n";
    }

    oss << "=== User Question ===\n";
    oss << query << "\n\n";
    if (rewritten.rewritten && rewritten.search_query != query) {
        oss << "=== Search Query Used For Retrieval ===\n";
        oss << rewritten.combined_query() << "\n\n";
    }
    oss << "Please answer based on the reference documents above. "
        << "If the documents don't contain relevant information, say so.\n";

    return oss.str();
}

std::vector<ParentResult> RagService::retrieve_parents(const std::string& query,
                                                         int top_k,
                                                         int tenant_id,
                                                         int oversample) {
    return retrieve_parents_prepared(rewrite_query(query), top_k, tenant_id, oversample);
}

std::vector<ParentResult> RagService::retrieve_parents_prepared(const QueryRewriteResult& query,
                                                                 int top_k,
                                                                 int tenant_id,
                                                                 int oversample) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto chunks = retrieve_prepared(query, top_k * oversample, tenant_id);
    if (chunks.empty()) return {};

    std::unordered_map<int, std::pair<int, float>> stats;
    for (auto& c : chunks) {
        auto& s = stats[c.doc_id];
        s.first++;
        s.second = std::max(s.second, c.score);
    }

    std::vector<std::pair<int, std::pair<int, float>>> ranked;
    for (auto& [doc_id, s] : stats) {
        ranked.push_back({doc_id, s});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                  if (a.second.first != b.second.first)
                      return a.second.first > b.second.first;
                  return a.second.second > b.second.second;
              });

    std::vector<ParentResult> results;
    for (size_t i = 0; i < std::min(ranked.size(), (size_t)top_k); ++i) {
        int doc_id = ranked[i].first;
        auto it = documents_.find(doc_id);
        if (it == documents_.end()) continue;

        ParentResult p;
        p.doc_id = doc_id;
        p.filename = it->second.filename;
        p.content = it->second.content;
        p.matched_chunks = ranked[i].second.first;
        p.max_score = ranked[i].second.second;

        for (auto& c : chunks) {
            if (c.doc_id == doc_id) {
                p.metadata = c.metadata;
                break;
            }
        }
        results.push_back(p);
    }

    return results;
}

std::string RagService::build_rag_prompt_from_parents(const std::string& query,
                                                         int tenant_id,
                                                         int top_k) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto rewritten = rewrite_query(query);
    auto parents = retrieve_parents_prepared(rewritten, top_k, tenant_id);
    if (parents.empty()) return query;

    std::ostringstream oss;
    oss << "Based on the following reference documents, answer the user's question.\n\n";
    oss << "=== Reference Documents ===\n\n";

    for (size_t i = 0; i < parents.size(); ++i) {
        auto& p = parents[i];
        std::string filename = p.filename.empty()
            ? ("document #" + std::to_string(p.doc_id))
            : p.filename;
        oss << "[" << (i + 1) << "] " << filename
            << " (" << p.matched_chunks << " matching sections"
            << ", best score: " << std::fixed << std::setprecision(3) << p.max_score << ")\n";
        oss << p.content << "\n\n";
    }

    oss << "=== User Question ===\n";
    oss << query << "\n\n";
    if (rewritten.rewritten && rewritten.search_query != query) {
        oss << "=== Search Query Used For Retrieval ===\n";
        oss << rewritten.combined_query() << "\n\n";
    }
    oss << "Please answer based on the reference documents above. "
        << "If the documents don't contain relevant information, say so.\n";

    return oss.str();
}

size_t RagService::document_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return documents_.size();
}

bool RagService::is_ready() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return embedder_ && embedder_->is_loaded();
}

}
