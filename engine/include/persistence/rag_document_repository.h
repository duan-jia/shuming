#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "persistence/mysql_pool.h"

namespace fastllm {

struct RagDocumentRecord {
    int id = 0;
    int tenant_id = 0;
    std::string filename;
    std::string content;
    std::string visibility = "group";
    int uploaded_by = 0;
    int chunk_count = 0;
};

struct RagChunkRecord {
    int64_t id = 0;
    int document_id = 0;
    int tenant_id = 0;
    int chunk_index = 0;
    std::string content;
    std::string visibility = "group";
};

class RagDocumentRepository {
public:
    explicit RagDocumentRepository(MysqlPool& mysql);

    std::vector<RagDocumentRecord> load_ready_documents();
    std::vector<RagChunkRecord> load_ready_chunks();
    int insert_processing_document(int tenant_id, const std::string& filename,
                                   const std::string& content,
                                   const std::string& visibility,
                                   int uploaded_by,
                                   const std::string& source_filename = "",
                                   const std::string& source_storage_name = "",
                                   std::uint64_t source_size_bytes = 0);
    bool insert_chunk(int document_id, int tenant_id, int chunk_index,
                      const std::string& content, const std::string& visibility,
                      int64_t& chunk_id);
    bool update_document_status(int doc_id, const std::string& status, int chunk_count);
    bool mark_document_deleted(int doc_id);

private:
    MysqlPool& mysql_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
