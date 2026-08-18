#include "services/document_service.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <regex>

namespace fastllm {

namespace {

bool can_delete_document(const DocumentSummary& doc, const UserInfo& user) {
    return doc.tenant_id == user.tenant_id
        && (user.role == Role::ADMIN || doc.uploaded_by == user.id);
}

nlohmann::json summary_json(const DocumentSummary& doc, const UserInfo& user) {
    nlohmann::json result;
    result["id"] = doc.id;
    result["tenant_id"] = doc.tenant_id;
    result["filename"] = doc.filename;
    result["chunk_count"] = doc.chunk_count;
    result["visibility"] = doc.visibility;
    result["status"] = doc.status;
    result["uploaded_by"] = doc.uploaded_by;
    result["uploader_username"] = doc.uploader_username;
    result["source_filename"] = doc.source_filename.empty()
        ? nlohmann::json(nullptr) : nlohmann::json(doc.source_filename);
    result["source_size_bytes"] = doc.source_size_bytes;
    result["markdown_size_bytes"] = doc.markdown_size_bytes;
    result["has_pdf"] = !doc.source_filename.empty();
    result["can_delete"] = can_delete_document(doc, user);
    result["created_at"] = doc.created_at;
    result["updated_at"] = doc.updated_at;
    return result;
}

bool parse_document_id(const RouteParams& params, int& doc_id) {
    const std::string value = params.get("id");
    if (value.empty()) return false;
    try {
        size_t consumed = 0;
        doc_id = std::stoi(value, &consumed);
        return consumed == value.size() && doc_id > 0;
    } catch (...) {
        return false;
    }
}

}

DocumentService::DocumentService(AppContext& ctx) : ctx_(ctx) {}

void DocumentService::upload(const HttpRequest& req, const UserInfo& user, HttpResponse& resp) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string filename = body.value("filename", "");
        std::string content = body.value("content", "");
        std::string visibility = body.value("visibility", "group");
        std::string source_filename = body.value("source_filename", "");
        std::string source_storage_name = body.value("source_storage_name", "");
        std::uint64_t source_size_bytes = body.value(
            "source_size_bytes", static_cast<std::uint64_t>(0));

        if (filename.empty() || content.empty()) {
            resp.json_error(400, "filename and content required");
            return;
        }

        if (visibility != "group" && visibility != "public") {
            resp.json_error(400, "visibility must be 'group' or 'public'");
            return;
        }

        bool has_source_metadata = !source_filename.empty()
            || !source_storage_name.empty()
            || source_size_bytes > 0;
        static const std::regex storage_name_pattern(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.pdf$");
        static const std::regex source_filename_pattern("^.+\\.[pP][dD][fF]$");
        if (has_source_metadata
            && (source_filename.empty()
                || source_storage_name.empty()
                || source_filename.size() > 256
                || source_filename.find('/') != std::string::npos
                || source_filename.find('\\') != std::string::npos
                || !std::regex_match(source_filename, source_filename_pattern)
                || !std::regex_match(source_storage_name, storage_name_pattern)
                || source_size_bytes == 0
                || source_size_bytes > 30ULL * 1024ULL * 1024ULL)) {
            resp.json_error(400, "Invalid source PDF metadata");
            return;
        }

        if (!ctx_.rag || !ctx_.rag->is_ready()) {
            resp.json_error(503, "RAG service not available (no embedding model)");
            return;
        }

        int doc_id = ctx_.rag->enqueue_document(
            user.tenant_id, filename, content, "", visibility, user.id,
            source_filename, source_storage_name, source_size_bytes);
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
            result.push_back(summary_json(doc, user));
        }
    }

    nlohmann::json j;
    j["documents"] = result;
    resp.json(j.dump());
}

void DocumentService::detail(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    int doc_id = 0;
    if (!parse_document_id(params, doc_id)) {
        resp.json_error(400, "Valid document ID required");
        return;
    }

    DocumentDetails document;
    if (!ctx_.document_repository
        || !ctx_.document_repository->find_visible_document(doc_id, user.tenant_id, document)) {
        resp.json_error(404, "Document not found");
        return;
    }

    nlohmann::json result = summary_json(document, user);
    result["source_storage_name"] = document.source_storage_name.empty()
        ? nlohmann::json(nullptr) : nlohmann::json(document.source_storage_name);
    result["content"] = document.content;
    resp.json(result.dump());
}

void DocumentService::remove(const RouteParams& params, const UserInfo& user, HttpResponse& resp) {
    int doc_id = 0;
    if (!parse_document_id(params, doc_id)) {
        resp.json_error(400, "Valid document ID required");
        return;
    }

    int doc_tenant_id = 0;
    int uploaded_by = 0;
    if (!ctx_.document_repository
        || !ctx_.document_repository->find_document_owner(doc_id, doc_tenant_id, uploaded_by)) {
        resp.json_error(404, "Document not found");
        return;
    }
    if (doc_tenant_id != user.tenant_id
        || (user.role != Role::ADMIN && uploaded_by != user.id)) {
        resp.json_error(403, "Forbidden");
        return;
    }

    if (!ctx_.rag) {
        resp.json_error(503, "RAG service not available");
        return;
    }
    if (!ctx_.rag->remove_document(doc_id)) {
        resp.json_error(500, "Document delete failed");
        return;
    }

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
