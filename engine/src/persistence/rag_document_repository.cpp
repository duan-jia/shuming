#include "persistence/rag_document_repository.h"

#include <mysql/mysql.h>
#include <spdlog/spdlog.h>

namespace fastllm {

RagDocumentRepository::RagDocumentRepository(MysqlPool& mysql) : mysql_(mysql) {}

std::string RagDocumentRepository::escape_sql(MYSQL* mysql, const std::string& input) {
    std::string output(input.size() * 2 + 1, '\0');
    size_t len = mysql_real_escape_string(mysql, &output[0], input.c_str(), input.size());
    output.resize(len);
    return output;
}

std::vector<RagDocumentRecord> RagDocumentRepository::load_ready_documents() {
    std::vector<RagDocumentRecord> documents;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return documents;

    auto* res = conn->query(
        "SELECT id, tenant_id, filename, content, visibility, uploaded_by, chunk_count "
        "FROM documents WHERE status='ready'");
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            RagDocumentRecord doc;
            doc.id = row[0] ? std::stoi(row[0]) : 0;
            doc.tenant_id = row[1] ? std::stoi(row[1]) : 0;
            doc.filename = row[2] ? row[2] : "";
            doc.content = row[3] ? row[3] : "";
            doc.visibility = row[4] ? row[4] : "group";
            doc.uploaded_by = row[5] ? std::stoi(row[5]) : 0;
            doc.chunk_count = row[6] ? std::stoi(row[6]) : 0;
            documents.push_back(doc);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return documents;
}

std::vector<RagChunkRecord> RagDocumentRepository::load_ready_chunks() {
    std::vector<RagChunkRecord> chunks;
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return chunks;

    auto* res = conn->query(
        "SELECT id, document_id, tenant_id, chunk_index, content, visibility "
        "FROM document_chunks dc WHERE EXISTS "
        "(SELECT 1 FROM documents d WHERE d.id = dc.document_id AND d.status='ready')");
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            RagChunkRecord chunk;
            chunk.id = row[0] ? std::stoll(row[0]) : 0;
            chunk.document_id = row[1] ? std::stoi(row[1]) : 0;
            chunk.tenant_id = row[2] ? std::stoi(row[2]) : 0;
            chunk.chunk_index = row[3] ? std::stoi(row[3]) : 0;
            chunk.content = row[4] ? row[4] : "";
            chunk.visibility = row[5] ? row[5] : "group";
            chunks.push_back(chunk);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    mysql_.release(conn);
    return chunks;
}

int RagDocumentRepository::insert_processing_document(int tenant_id,
                                                      const std::string& filename,
                                                      const std::string& content,
                                                      const std::string& visibility,
                                                      int uploaded_by,
                                                      const std::string& source_filename,
                                                      const std::string& source_storage_name,
                                                      std::uint64_t source_size_bytes,
                                                      const std::string& source_sha256,
                                                      bool needs_review,
                                                      const std::string& review_reason) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) {
        spdlog::error("Cannot acquire MySQL connection");
        return -1;
    }

    std::string source_filename_sql = source_filename.empty()
        ? "NULL" : "'" + escape_sql(conn->raw(), source_filename) + "'";
    std::string source_storage_sql = source_storage_name.empty()
        ? "NULL" : "'" + escape_sql(conn->raw(), source_storage_name) + "'";
    std::string source_size_sql = source_filename.empty()
        ? "NULL" : std::to_string(source_size_bytes);
    std::string source_sha256_sql = source_sha256.empty()
        ? "NULL" : "'" + escape_sql(conn->raw(), source_sha256) + "'";
    std::string sql = "INSERT INTO documents (tenant_id, filename, content, visibility, uploaded_by, "
        "source_filename, source_storage_name, source_size_bytes, source_sha256, review_reason, status) VALUES ("
        + std::to_string(tenant_id) + ",'"
        + escape_sql(conn->raw(), filename) + "','"
        + escape_sql(conn->raw(), content) + "','"
        + escape_sql(conn->raw(), visibility) + "',"
        + std::to_string(uploaded_by) + ","
        + source_filename_sql + ","
        + source_storage_sql + ","
        + source_size_sql + ","
        + source_sha256_sql + ",'"
        + escape_sql(conn->raw(), review_reason) + "','"
        + (needs_review ? "pending" : "processing") + "')";
    if (!conn->execute(sql)) {
        int error_code = mysql_errno(conn->raw());
        mysql_.release(conn);
        if (error_code == 1062) return -2;
        spdlog::error("Failed to insert document");
        return -1;
    }
    int doc_id = static_cast<int>(mysql_insert_id(conn->raw()));
    mysql_.release(conn);
    return doc_id;
}

bool RagDocumentRepository::insert_chunk(int document_id, int tenant_id, int chunk_index,
                                         const std::string& content,
                                         const std::string& visibility,
                                         int64_t& chunk_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "INSERT INTO document_chunks (document_id, tenant_id, chunk_index, content, visibility) VALUES ("
        + std::to_string(document_id) + ","
        + std::to_string(tenant_id) + ","
        + std::to_string(chunk_index) + ",'"
        + escape_sql(conn->raw(), content) + "','"
        + escape_sql(conn->raw(), visibility) + "')";
    bool ok = conn->execute(sql);
    if (ok) {
        chunk_id = static_cast<int64_t>(mysql_insert_id(conn->raw()));
    }
    mysql_.release(conn);
    return ok;
}

bool RagDocumentRepository::update_document_status(int doc_id, const std::string& status,
                                                   int chunk_count) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "UPDATE documents SET status='"
        + escape_sql(conn->raw(), status) + "', chunk_count="
        + std::to_string(chunk_count)
        + (status == "failed" ? ", source_sha256=NULL" : "")
        + " WHERE id=" + std::to_string(doc_id);
    bool ok = conn->execute(sql);
    mysql_.release(conn);
    return ok;
}

bool RagDocumentRepository::mark_document_processing(int doc_id, int tenant_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "UPDATE documents SET status='processing' WHERE id="
        + std::to_string(doc_id) + " AND tenant_id=" + std::to_string(tenant_id)
        + " AND status='pending'";
    bool ok = conn->execute(sql) && mysql_affected_rows(conn->raw()) == 1;
    mysql_.release(conn);
    return ok;
}

bool RagDocumentRepository::mark_document_deleted(int doc_id) {
    auto conn = mysql_.acquire();
    if (!conn || !conn->valid()) return false;

    std::string sql = "UPDATE documents SET status='deleted', source_sha256=NULL WHERE id="
        + std::to_string(doc_id);
    bool ok = conn->execute(sql);
    mysql_.release(conn);
    return ok;
}

}
