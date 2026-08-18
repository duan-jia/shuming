#include "persistence/document_repository.h"

#include <mysql/mysql.h>

namespace fastllm {

DocumentRepository::DocumentRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::vector<DocumentSummary> DocumentRepository::list_visible_documents(int tenant_id) {
    std::vector<DocumentSummary> documents;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return documents;

    std::string sql =
        "SELECT d.id, d.tenant_id, d.filename, d.chunk_count, d.visibility, d.status, "
        "d.uploaded_by, COALESCE(u.username,''), COALESCE(d.source_filename,''), "
        "COALESCE(d.source_size_bytes,0), OCTET_LENGTH(d.content), d.created_at, d.updated_at "
        "FROM documents d LEFT JOIN users u ON u.id=d.uploaded_by "
        "WHERE d.status<>'deleted' AND (d.tenant_id="
        + std::to_string(tenant_id) + " OR d.visibility='public') ORDER BY d.created_at DESC";
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
            doc.uploaded_by = row[6] ? std::stoi(row[6]) : 0;
            doc.uploader_username = row[7] ? row[7] : "";
            doc.source_filename = row[8] ? row[8] : "";
            doc.source_size_bytes = row[9] ? std::stoull(row[9]) : 0;
            doc.markdown_size_bytes = row[10] ? std::stoull(row[10]) : 0;
            doc.created_at = row[11] ? row[11] : "";
            doc.updated_at = row[12] ? row[12] : "";
            documents.push_back(doc);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return documents;
}

bool DocumentRepository::find_visible_document(int doc_id, int tenant_id,
                                               DocumentDetails& document) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql =
        "SELECT d.id, d.tenant_id, d.filename, d.chunk_count, d.visibility, d.status, "
        "d.uploaded_by, COALESCE(u.username,''), COALESCE(d.source_filename,''), "
        "COALESCE(d.source_size_bytes,0), OCTET_LENGTH(d.content), d.created_at, d.updated_at, "
        "COALESCE(d.source_storage_name,''), d.content "
        "FROM documents d LEFT JOIN users u ON u.id=d.uploaded_by "
        "WHERE d.id=" + std::to_string(doc_id)
        + " AND d.status<>'deleted' AND (d.tenant_id=" + std::to_string(tenant_id)
        + " OR d.visibility='public') LIMIT 1";
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

    document.id = row[0] ? std::stoi(row[0]) : 0;
    document.tenant_id = row[1] ? std::stoi(row[1]) : 0;
    document.filename = row[2] ? row[2] : "";
    document.chunk_count = row[3] ? std::stoi(row[3]) : 0;
    document.visibility = row[4] ? row[4] : "group";
    document.status = row[5] ? row[5] : "processing";
    document.uploaded_by = row[6] ? std::stoi(row[6]) : 0;
    document.uploader_username = row[7] ? row[7] : "";
    document.source_filename = row[8] ? row[8] : "";
    document.source_size_bytes = row[9] ? std::stoull(row[9]) : 0;
    document.markdown_size_bytes = row[10] ? std::stoull(row[10]) : 0;
    document.created_at = row[11] ? row[11] : "";
    document.updated_at = row[12] ? row[12] : "";
    document.source_storage_name = row[13] ? row[13] : "";
    document.content = row[14] ? row[14] : "";

    mysql_free_result(res);
    mysql_.release(conn);
    return true;
}

bool DocumentRepository::find_document_owner(int doc_id, int& tenant_id, int& uploaded_by) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "SELECT tenant_id, uploaded_by FROM documents WHERE id="
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
    uploaded_by = row[1] ? std::stoi(row[1]) : 0;
    mysql_free_result(res);
    mysql_.release(conn);
    return true;
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
