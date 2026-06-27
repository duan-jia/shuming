#include "persistence/document_repository.h"

#include <mysql/mysql.h>

namespace fastllm {

DocumentRepository::DocumentRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::vector<DocumentSummary> DocumentRepository::list_visible_documents(int tenant_id) {
    std::vector<DocumentSummary> documents;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return documents;

    std::string sql = "SELECT id, tenant_id, filename, chunk_count, visibility, status, created_at "
        "FROM documents WHERE status<>'deleted' AND (tenant_id="
        + std::to_string(tenant_id) + " OR visibility='public') ORDER BY created_at DESC";
    auto* res = conn->query(sql);
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            DocumentSummary doc;
            doc.id = row[0] ? std::stoi(row[0]) : 0;
            doc.tenant_id = row[1] ? std::stoi(row[1]) : 0;
            doc.filename = row[2] ? row[2] : "";
            doc.chunk_count = row[3] ? std::stoi(row[3]) : 0;
            doc.visibility = row[4] ? row[4] : "group";
            doc.status = row[5] ? row[5] : "processing";
            doc.created_at = row[6] ? row[6] : "";
            documents.push_back(doc);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return documents;
}

bool DocumentRepository::find_document_tenant(int doc_id, int& tenant_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "SELECT tenant_id FROM documents WHERE id="
        + std::to_string(doc_id) + " AND status<>'deleted' LIMIT 1";
    auto* res = conn->query(sql);
    if (!res) {
        mysql_.release(conn);
        return false;
    }

    auto* row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        mysql_.release(conn);
        return false;
    }

    tenant_id = row[0] ? std::stoi(row[0]) : 0;
    mysql_free_result(res);
    mysql_.release(conn);
    return true;
}

}
