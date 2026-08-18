#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>

#include "rag/chunker.h"
#include "rag/embedder.h"
#include "rag/vector_index.h"
#include "rag/bm25_index.h"
#include "rag/query_rewriter.h"
#include "persistence/mysql_pool.h"
#include "persistence/rag_document_repository.h"

namespace fastllm {

struct Document {
    int id = 0;
    int tenant_id = 0;
    std::string filename;
    std::string content;
    std::string visibility = "group";
    int uploaded_by = 0;
    int chunk_count = 0;
};

struct ParentResult {
    int doc_id;
    std::string filename;
    std::string content;
    ChunkMetadata metadata;
    int matched_chunks = 0;
    float max_score = 0.0f;
};

class RagService {
public:
    RagService();
    ~RagService();

    void set_embedder(std::unique_ptr<Embedder> embedder);
    bool load_embedding_model(const std::string& model_path);
    void set_mysql_pool(MysqlPool* pool);
    void set_query_rewriter(std::unique_ptr<QueryRewriter> query_rewriter);
    QueryRewriteResult rewrite_query(const std::string& query) const;

    int add_document(int tenant_id, const std::string& filename,
                     const std::string& content, const std::string& category = "",
                     const std::string& visibility = "group",
                     int uploaded_by = 0,
                     bool save = true,
                     const std::string& source_filename = "",
                     const std::string& source_storage_name = "",
                     std::uint64_t source_size_bytes = 0);
    int enqueue_document(int tenant_id, const std::string& filename,
                         const std::string& content, const std::string& category = "",
                         const std::string& visibility = "group",
                         int uploaded_by = 0,
                         const std::string& source_filename = "",
                         const std::string& source_storage_name = "",
                         std::uint64_t source_size_bytes = 0);
    bool remove_document(int doc_id);

    std::vector<SearchResult> retrieve(const std::string& query,
                                        int top_k = 5,
                                        int tenant_id = -1);

    std::vector<SearchResult> retrieve_prepared(const QueryRewriteResult& query,
                                                int top_k = 5,
                                                int tenant_id = -1);

    std::vector<SearchResult> vector_only_search(const std::string& query,
                                                  int top_k = 5,
                                                  int tenant_id = -1);

    std::vector<SearchResult> bm25_only_search(const std::string& query, int top_k = 5,
                                                int tenant_id = -1);

    std::vector<ParentResult> retrieve_parents(const std::string& query,
                                                int top_k = 5,
                                                int tenant_id = -1,
                                                int oversample = 3);

    std::vector<ParentResult> retrieve_parents_prepared(const QueryRewriteResult& query,
                                                         int top_k = 5,
                                                         int tenant_id = -1,
                                                         int oversample = 3);

    std::string build_rag_prompt(const std::string& query,
                                  int tenant_id = -1,
                                  int top_k = 3);

    std::string build_rag_prompt_from_parents(const std::string& query,
                                               int tenant_id = -1,
                                               int top_k = 3);

    size_t document_count() const;
    bool is_ready() const;

    void set_storage_dir(const std::string& dir);
    bool save_all();
    bool load_all();

private:
    struct DocumentJob {
        int doc_id = 0;
        int tenant_id = 0;
        std::string filename;
        std::string content;
        std::string category;
        std::string visibility = "group";
        int uploaded_by = 0;
    };

    void start_worker();
    void stop_worker();
    void worker_loop();
    bool process_document_job(const DocumentJob& job, bool save);
    int insert_processing_document(int tenant_id, const std::string& filename,
                                   const std::string& content,
                                   const std::string& visibility,
                                   int uploaded_by,
                                   const std::string& source_filename = "",
                                   const std::string& source_storage_name = "",
                                   std::uint64_t source_size_bytes = 0);

    Chunker chunker_;
    std::unique_ptr<Embedder> embedder_;
    std::unique_ptr<QueryRewriter> query_rewriter_;
    VectorIndex index_;
    Bm25Index bm25_;
    std::unordered_map<int, Document> documents_;
    std::unordered_set<int> deleted_documents_;
    MysqlPool* mysql_pool_ = nullptr;
    std::unique_ptr<RagDocumentRepository> document_repository_;
    std::string storage_dir_;
    mutable std::recursive_mutex mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<DocumentJob> jobs_;
    std::thread worker_;
    bool worker_started_ = false;
    bool stopping_ = false;
};

}
