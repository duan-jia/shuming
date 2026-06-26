#include "services/document_service.h"

#include <mysql/mysql.h>
#include <nlohmann/json.hpp>

namespace fastllm {

DocumentService::DocumentService(AppContext& ctx) : ctx_(ctx) {}

void DocumentService::upload(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string filename = body.value("filename", "");
        std::string content = body.value("content", "");
        std::string visibility = body.value("visibility", "group");

        if (filename.empty() || content.empty()) {
            resp.json_error(400, "filename and content required");
            return;
        }

        if (visibility != "group" && visibility != "public") {
            resp.json_error(400, "visibility must be 'group' or 'public'");
            return;
        }

        if (!ctx_.rag || !ctx_.rag->is_ready()) {
            resp.json_error(503, "RAG service not available (no embedding model)");
            return;
        }

        int doc_id = ctx_.rag->enqueue_document(user.tenant_id, filename, content, "", visibility, user.id);
        if (doc_id <= 0) {
            resp.json_error(500, "Document enqueue failed");
            return;
        }

        nlohmann::json j;
        j["id"] = doc_id;
        j["filename"] = filename;
        j["visibility"] = visibility;
        j["status"] = "processing";
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

void DocumentService::list(const UserInfo& user, HttpResponse& resp) {
    auto conn = ctx_.mysql_pool->acquire();
    if (!conn) { resp.json_error(500, "Database error"); return; }

    nlohmann::json result = nlohmann::json::array();
    std::string sql = "SELECT id, tenant_id, filename, chunk_count, visibility, status, created_at "
        "FROM documents WHERE status<>'deleted' AND (tenant_id="
        + std::to_string(user.tenant_id) + " OR visibility='public') ORDER BY created_at DESC";
    auto* res = conn->query(sql);
    if (res) {
        auto* row = mysql_fetch_row(res);
        while (row) {
            nlohmann::json d;
            d["id"] = row[0] ? std::stoi(row[0]) : 0;
            d["tenant_id"] = row[1] ? std::stoi(row[1]) : 0;
            d["filename"] = row[2] ? row[2] : "";
            d["chunk_count"] = row[3] ? std::stoi(row[3]) : 0;
            d["visibility"] = row[4] ? row[4] : "group";
            d["status"] = row[5] ? row[5] : "processing";
            d["created_at"] = row[6] ? row[6] : "";
            result.push_back(d);
            row = mysql_fetch_row(res);
        }
        mysql_free_result(res);
    }
    ctx_.mysql_pool->release(conn);

    nlohmann::json j;
    j["documents"] = result;
    resp.json(j.dump());
}

void DocumentService::remove(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    if (user.role != Role::ADMIN) {
        resp.json_error(403, "Admin only");
        return;
    }

    std::string doc_id_str = params.get("id");
    if (doc_id_str.empty()) {
        resp.json_error(400, "Document ID required");
        return;
    }

    int doc_id = std::stoi(doc_id_str);
    ctx_.rag->remove_document(doc_id);

    resp.json(R"({"status":"deleted"})");
}

void DocumentService::search(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string query = body.value("query", "");
        int top_k = body.value("top_k", 5);
        std::string mode = body.value("mode", "chunks");

        if (query.empty()) {
            resp.json_error(400, "query required");
            return;
        }

        if (!ctx_.rag || !ctx_.rag->is_ready()) {
            resp.json_error(503, "RAG service not available");
            return;
        }

        nlohmann::json result_arr = nlohmann::json::array();

        if (mode == "parents") {
            auto results = ctx_.rag->retrieve_parents(query, top_k, user.tenant_id);
            for (auto& p : results) {
                nlohmann::json j;
                j["doc_id"] = p.doc_id;
                j["filename"] = p.filename;
                j["content"] = p.content;
                j["matched_chunks"] = p.matched_chunks;
                j["max_score"] = p.max_score;
                result_arr.push_back(j);
            }
        } else {
            auto results = ctx_.rag->retrieve(query, top_k, user.tenant_id);
            for (auto& r : results) {
                nlohmann::json j;
                j["doc_id"] = r.doc_id;
                j["chunk_index"] = r.chunk_index;
                j["score"] = r.score;
                j["content"] = r.content;
                result_arr.push_back(j);
            }
        }

        nlohmann::json j;
        j["results"] = result_arr;
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

}
