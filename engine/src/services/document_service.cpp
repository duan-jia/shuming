#include "services/document_service.h"

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
    nlohmann::json result = nlohmann::json::array();
    if (ctx_.document_repository) {
        for (const auto& doc : ctx_.document_repository->list_visible_documents(user.tenant_id)) {
            nlohmann::json d;
            d["id"] = doc.id;
            d["tenant_id"] = doc.tenant_id;
            d["filename"] = doc.filename;
            d["chunk_count"] = doc.chunk_count;
            d["visibility"] = doc.visibility;
            d["status"] = doc.status;
            d["created_at"] = doc.created_at;
            result.push_back(d);
        }
    }

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
    int doc_tenant_id = 0;
    if (!ctx_.document_repository
        || !ctx_.document_repository->find_document_tenant(doc_id, doc_tenant_id)) {
        resp.json_error(404, "Document not found");
        return;
    }
    if (doc_tenant_id != user.tenant_id) {
        resp.json_error(403, "Forbidden");
        return;
    }

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
        auto rewritten = ctx_.rag->rewrite_query(query);

        if (mode == "parents") {
            auto results = ctx_.rag->retrieve_parents_prepared(rewritten, top_k, user.tenant_id);
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
            auto results = ctx_.rag->retrieve_prepared(rewritten, top_k, user.tenant_id);
            for (auto& r : results) {
                nlohmann::json j;
                j["doc_id"] = r.doc_id;
                j["filename"] = r.filename;
                j["chunk_index"] = r.chunk_index;
                j["score"] = r.score;
                j["content"] = r.content;
                result_arr.push_back(j);
            }
        }

        nlohmann::json j;
        j["query"] = query;
        j["rewritten_query"] = rewritten.search_query;
        j["query_rewritten"] = rewritten.rewritten;
        j["query_aliases"] = rewritten.aliases;
        if (!rewritten.failure_reason.empty()) {
            j["rewrite_failure_reason"] = rewritten.failure_reason;
        }
        j["results"] = result_arr;
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

}
