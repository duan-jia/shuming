#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "persistence/mysql_pool.h"

namespace fastllm {

struct DocumentSummary {
    int id = 0;
    int tenant_id = 0;
    std::string filename;
    int chunk_count = 0;
    std::string visibility = "group";
    std::string status = "processing";
    int uploaded_by = 0;
    std::string uploader_username;
    std::string source_filename;
    std::string review_reason;
    std::uint64_t source_size_bytes = 0;
    std::uint64_t markdown_size_bytes = 0;
    std::string created_at;
    std::string updated_at;
};

struct DocumentDetails : DocumentSummary {
    std::string source_storage_name;
    std::string content;
};

class DocumentRepository {
public:
    explicit DocumentRepository(MysqlPool& mysql);

    std::vector<DocumentSummary> list_visible_documents(int tenant_id);
    bool find_visible_document(int doc_id, int tenant_id, DocumentDetails& document);
    bool find_document_owner(int doc_id, int& tenant_id, int& uploaded_by);
    bool find_document_tenant(int doc_id, int& tenant_id);
    bool find_duplicate_document(int tenant_id, const std::string& source_sha256,
                                 DocumentSummary& document);

private:
    MysqlPool& mysql_;
};

}
