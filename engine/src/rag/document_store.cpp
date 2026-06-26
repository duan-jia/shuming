#include "rag/document_store.h"
#include "rag/query_filter.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace fastllm {

static std::string escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

RagService::RagService() : chunker_(Chunker::Config{}), storage_dir_("data/rag") {}

RagService::~RagService() {
    stop_worker();
    save_all();
}

void RagService::set_storage_dir(const std::string& dir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    storage_dir_ = dir;
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

    if (!mysql_pool_) {
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

        auto conn = mysql_pool_->acquire();
        if (!conn) {
            spdlog::error("RagService: cannot acquire MySQL connection for load");
            return false;
        }
        MYSQL* raw = conn->raw();

        // Load documents (only ready ones)
        documents_.clear();
        auto* res = conn->query("SELECT id, tenant_id, filename, content, visibility, uploaded_by, chunk_count FROM documents WHERE status='ready'");
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                Document doc;
                doc.id = std::stoi(row[0]);
                doc.tenant_id = std::stoi(row[1]);
                doc.filename = row[2] ? row[2] : "";
                doc.content = row[3] ? row[3] : "";
                doc.visibility = row[4] ? row[4] : "group";
                doc.uploaded_by = row[5] ? std::stoi(row[5]) : 0;
                doc.chunk_count = row[6] ? std::stoi(row[6]) : 0;
                documents_[doc.id] = doc;
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }

        // Load chunks → restore VectorEntry in memory
        int chunk_count = 0;
        res = conn->query("SELECT id, document_id, tenant_id, chunk_index, content, visibility FROM document_chunks dc WHERE EXISTS (SELECT 1 FROM documents d WHERE d.id = dc.document_id AND d.status='ready')");
        if (res) {
            auto* row = mysql_fetch_row(res);
            while (row) {
                int64_t chunk_id = std::stoll(row[0]);
                VectorEntry entry;
                entry.doc_id = std::stoi(row[1]);
                entry.tenant_id = std::stoi(row[2]);
                entry.chunk_index = std::stoi(row[3]);
                entry.content = row[4] ? row[4] : "";
                entry.visibility = row[5] ? row[5] : "group";
                index_.restore_entry(static_cast<faiss::idx_t>(chunk_id), entry);
                chunk_count++;
                row = mysql_fetch_row(res);
            }
            mysql_free_result(res);
        }

        mysql_pool_->release(conn);

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
                                           int uploaded_by) {
    if (!mysql_pool_) {
        spdlog::error("MySQL pool not set, cannot insert document");
        return -1;
    }

    auto conn = mysql_pool_->acquire();
    if (!conn) {
        spdlog::error("Cannot acquire MySQL connection");
        return -1;
    }
    MYSQL* raw = conn->raw();

    // 1. INSERT document with status='processing'
    std::string sql = "INSERT INTO documents (tenant_id, filename, content, visibility, uploaded_by, status) VALUES ("
        + std::to_string(tenant_id) + ",'"
        + escape_sql(raw, filename) + "','"
        + escape_sql(raw, content) + "','"
        + escape_sql(raw, visibility) + "',"
        + std::to_string(uploaded_by) + ",'processing')";
    if (!conn->execute(sql)) {
        mysql_pool_->release(conn);
        spdlog::error("Failed to insert document");
        return -1;
    }
    int doc_id = static_cast<int>(mysql_insert_id(raw));
    mysql_pool_->release(conn);
    return doc_id;
}

bool RagService::process_document_job(const DocumentJob& job, bool save) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_ || !embedder_->is_loaded()) {
        spdlog::error("Embedding model not loaded, cannot process document {}", job.doc_id);
        return false;
    }

    if (!mysql_pool_) {
        spdlog::error("MySQL pool not set, cannot process document {}", job.doc_id);
        return false;
    }

    auto conn = mysql_pool_->acquire();
    if (!conn) {
        spdlog::error("Cannot acquire MySQL connection");
        return false;
    }
    MYSQL* raw = conn->raw();

    // 2. Chunk the content
    auto chunks = chunker_.chunk(job.doc_id, job.content, job.category);

    int indexed_count = 0;
    std::string sql;

    // 3. For each chunk: INSERT into document_chunks -> embed -> add to FAISS + BM25
    for (auto& chunk : chunks) {
        sql = "INSERT INTO document_chunks (document_id, tenant_id, chunk_index, content, visibility) VALUES ("
            + std::to_string(job.doc_id) + ","
            + std::to_string(job.tenant_id) + ","
            + std::to_string(chunk.chunk_index) + ",'"
            + escape_sql(raw, chunk.content) + "','"
            + escape_sql(raw, job.visibility) + "')";
        if (!conn->execute(sql)) {
            spdlog::warn("Failed to insert chunk {} of doc {}", chunk.chunk_index, job.doc_id);
            continue;
        }
        int64_t chunk_id = static_cast<int64_t>(mysql_insert_id(raw));

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
        known_dishes_.insert(chunk.metadata.dish);
        indexed_count++;
    }

    // 4. UPDATE document status
    std::string status = indexed_count > 0 ? "ready" : "failed";
    sql = "UPDATE documents SET status='" + status + "', chunk_count="
        + std::to_string(indexed_count) + " WHERE id=" + std::to_string(job.doc_id);
    conn->execute(sql);

    mysql_pool_->release(conn);

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
                               const std::string& visibility, int uploaded_by, bool save) {
    int doc_id = insert_processing_document(tenant_id, filename, content, visibility, uploaded_by);
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
                                 const std::string& visibility, int uploaded_by) {
    if (!embedder_ || !embedder_->is_loaded()) {
        spdlog::error("Embedding model not loaded, cannot enqueue document");
        return -1;
    }

    int doc_id = insert_processing_document(tenant_id, filename, content, visibility, uploaded_by);
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

void RagService::remove_document(int doc_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = documents_.find(doc_id);
    if (it == documents_.end()) return;

    index_.remove_by_doc(doc_id);
    bm25_.remove_by_doc(doc_id);
    spdlog::info("Document removed: id={}", doc_id);
    documents_.erase(it);

    // Soft delete in MySQL
    if (mysql_pool_) {
        auto conn = mysql_pool_->acquire();
        if (conn) {
            std::string sql = "UPDATE documents SET status='deleted' WHERE id=" + std::to_string(doc_id);
            conn->execute(sql);
            mysql_pool_->release(conn);
        }
    }

    save_all();
}

std::vector<SearchResult> RagService::vector_only_search(const std::string& query,
                                                           int top_k,
                                                           int tenant_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_ || !embedder_->is_loaded()) return {};
    auto emb = embedder_->embed(query);
    if (emb.empty()) return {};
    return index_.search(emb, top_k, tenant_id);
}

std::vector<SearchResult> RagService::bm25_only_search(const std::string& query, int top_k,
                                                        int tenant_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return bm25_.search(query, top_k, tenant_id);
}

std::vector<SearchResult> RagService::retrieve(const std::string& query, int top_k,
                                                 int tenant_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!embedder_ || !embedder_->is_loaded()) {
        spdlog::error("Embedding model not loaded");
        return {};
    }

    auto query_emb = embedder_->embed(query);
    if (query_emb.empty()) return {};

    int oversample = std::max(top_k, 5) * 3;
    auto vec_results = index_.search(query_emb, oversample, tenant_id);
    auto bm25_results = bm25_.search(query, oversample, tenant_id);

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

    auto filter = parse_query_filters(query, known_dishes_);

    std::vector<SearchResult> results;
    for (size_t i = 0; i < ranked.size(); ++i) {
        auto it = entry_map.find(ranked[i].first);
        if (it == entry_map.end()) continue;

        if (filter.active()) {
            const auto& meta = it->second->metadata;
            int stars = 0;
            for (size_t ci = 0; ci < meta.difficulty.size(); ) {
                unsigned char c = static_cast<unsigned char>(meta.difficulty[ci]);
                if (c == 0xE2 && ci + 2 < meta.difficulty.size() &&
                    static_cast<unsigned char>(meta.difficulty[ci+1]) == 0x98 &&
                    static_cast<unsigned char>(meta.difficulty[ci+2]) == 0x85) {
                    stars++;
                    ci += 3;
                } else {
                    ci++;
                }
            }
            if (filter.min_difficulty > 0 && stars < filter.min_difficulty) continue;
            if (filter.max_difficulty > 0 && stars > filter.max_difficulty) continue;
            if (!filter.dish_keyword.empty() &&
                meta.dish.find(filter.dish_keyword) == std::string::npos) continue;
            if (!filter.category.empty() && meta.category != filter.category) continue;
        }

        SearchResult r = *it->second;
        r.score = static_cast<float>(ranked[i].second);
        results.push_back(r);
        if (static_cast<int>(results.size()) >= top_k) break;
    }

    return results;
}

std::string RagService::build_rag_prompt(const std::string& query,
                                           int tenant_id,
                                           int top_k) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto results = retrieve(query, top_k, tenant_id);
    if (results.empty()) return query;

    std::ostringstream oss;
    oss << "Based on the following reference documents, answer the user's question.\n\n";
    oss << "=== Reference Documents ===\n\n";

    for (size_t i = 0; i < results.size(); ++i) {
        auto& r = results[i];
        oss << "[" << (i + 1) << "] " << r.metadata.category << "/"
            << r.metadata.dish << " " << r.metadata.difficulty
            << " (relevance: " << std::fixed << std::setprecision(3) << r.score << ")\n";
        oss << r.content << "\n\n";
    }

    oss << "=== User Question ===\n";
    oss << query << "\n\n";
    oss << "Please answer based on the reference documents above. "
        << "If the documents don't contain relevant information, say so.\n";

    return oss.str();
}

std::vector<ParentResult> RagService::retrieve_parents(const std::string& query,
                                                         int top_k,
                                                         int tenant_id,
                                                         int oversample) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto chunks = retrieve(query, top_k * oversample, tenant_id);
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
    auto parents = retrieve_parents(query, top_k, tenant_id);
    if (parents.empty()) return query;

    std::ostringstream oss;
    oss << "Based on the following reference documents, answer the user's question.\n\n";
    oss << "=== Reference Documents ===\n\n";

    for (size_t i = 0; i < parents.size(); ++i) {
        auto& p = parents[i];
        oss << "[" << (i + 1) << "] " << p.metadata.category << "/"
            << p.metadata.dish << " " << p.metadata.difficulty
            << " (" << p.matched_chunks << " matching sections"
            << ", best score: " << std::fixed << std::setprecision(3) << p.max_score << ")\n";
        oss << p.content << "\n\n";
    }

    oss << "=== User Question ===\n";
    oss << query << "\n\n";
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
