#pragma once

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
    std::string created_at;
};

class DocumentRepository {
public:
    explicit DocumentRepository(MysqlPool& mysql);

    std::vector<DocumentSummary> list_visible_documents(int tenant_id);
    bool find_document_tenant(int doc_id, int& tenant_id);

private:
    MysqlPool& mysql_;
};

}
