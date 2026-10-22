#include "services/tool_service.h"
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
    result["review_reason"] = doc.review_reason.empty()
        ? nlohmann::json(nullptr) : nlohmann::json(doc.review_reason);
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
        std::string source_sha256 = body.value("source_sha256", "");
        bool needs_review = body.value("needs_review", false);
        std::string review_reason = body.value("review_reason", "");

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
            || source_size_bytes > 0
            || !source_sha256.empty();
        static const std::regex storage_name_pattern(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}(--[^/\\\\]{1,180})?\\.pdf$");
        static const std::regex source_filename_pattern("^.+\\.[pP][dD][fF]$");
        static const std::regex sha256_pattern("^[0-9a-f]{64}$");
        if (has_source_metadata
            && (source_filename.empty()
                || source_storage_name.empty()
                || source_sha256.empty()
                || source_filename.size() > 256
                || source_filename.find('/') != std::string::npos
                || source_filename.find('\\') != std::string::npos
                || !std::regex_match(source_filename, source_filename_pattern)
                || !std::regex_match(source_storage_name, storage_name_pattern)
                || !std::regex_match(source_sha256, sha256_pattern)
                || source_size_bytes == 0
                || source_size_bytes > 30ULL * 1024ULL * 1024ULL)) {
            resp.json_error(400, "Invalid source PDF metadata");
            return;
        }
        if (review_reason.size() > 1000 || (needs_review && review_reason.empty())) {
            resp.json_error(400, "Invalid document review metadata");
            return;
        }

        DocumentSummary duplicate;
        if (!source_sha256.empty() && ctx_.document_repository
            && ctx_.document_repository->find_duplicate_document(
                user.tenant_id, source_sha256, duplicate)) {
            nlohmann::json j;
            j["error"] = "Document already exists";
            j["id"] = duplicate.id;
            j["filename"] = duplicate.filename;
            j["status"] = duplicate.status;
            resp.set_status(409, "Conflict");
            resp.json(j.dump());
            return;
        }

        if (!ctx_.rag || (!needs_review && !ctx_.rag->is_ready())) {
            resp.json_error(503, "RAG service not available (no embedding model)");
            return;
        }

        int doc_id = ctx_.rag->enqueue_document(
            user.tenant_id, filename, content, "", visibility, user.id,
            source_filename, source_storage_name, source_size_bytes,
            source_sha256, needs_review, review_reason);
        if (doc_id == -2) {
            DocumentSummary raced_duplicate;
            if (ctx_.document_repository
                && ctx_.document_repository->find_duplicate_document(
                    user.tenant_id, source_sha256, raced_duplicate)) {
                nlohmann::json j;
                j["error"] = "Document already exists";
                j["id"] = raced_duplicate.id;
                j["filename"] = raced_duplicate.filename;
                j["status"] = raced_duplicate.status;
                resp.set_status(409, "Conflict");
                resp.json(j.dump());
                return;
            }
        }
        if (doc_id <= 0) {
            resp.json_error(500, "Document enqueue failed");
            return;
        }

        nlohmann::json j;
        j["id"] = doc_id;
        j["filename"] = filename;
        j["visibility"] = visibility;
        j["status"] = needs_review ? "pending" : "processing";
        resp.json(j.dump());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

void DocumentService::deduplicate(const HttpRequest& req, const UserInfo& user,
                                  HttpResponse& resp) {
    try {
        auto body = nlohmann::json::parse(req.body);
        std::string source_sha256 = body.value("source_sha256", "");
        static const std::regex sha256_pattern("^[0-9a-f]{64}$");
        if (!std::regex_match(source_sha256, sha256_pattern)) {
            resp.json_error(400, "Valid source_sha256 required");
            return;
        }

        DocumentSummary duplicate;
        nlohmann::json j;
        if (ctx_.document_repository
            && ctx_.document_repository->find_duplicate_document(
                user.tenant_id, source_sha256, duplicate)) {
            j["duplicate"] = true;
            j["id"] = duplicate.id;
            j["filename"] = duplicate.filename;
            j["status"] = duplicate.status;
        } else {
            j["duplicate"] = false;
        }
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

void DocumentService::approve(const RouteParams& params, const UserInfo& user,
                              HttpResponse& resp) {
    if (user.role != Role::ADMIN) {
        resp.json_error(403, "Admin only");
        return;
    }

    int doc_id = 0;
    if (!parse_document_id(params, doc_id)) {
        resp.json_error(400, "Valid document ID required");
        return;
    }

    DocumentDetails document;
    if (!ctx_.document_repository
        || !ctx_.document_repository->find_visible_document(
            doc_id, user.tenant_id, document)
        || document.tenant_id != user.tenant_id) {
        resp.json_error(404, "Document not found");
        return;
    }
    if (document.status != "pending") {
        resp.json_error(409, "Document is not pending review");
        return;
    }
    if (!ctx_.rag || !ctx_.rag->is_ready()) {
        resp.json_error(503, "RAG service not available (no embedding model)");
        return;
    }
    if (!ctx_.rag->approve_document(
        document.id, document.tenant_id, document.filename, document.content,
        document.visibility, document.uploaded_by)) {
        resp.json_error(409, "Document approval failed or was already handled");
        return;
    }

    nlohmann::json j;
    j["id"] = document.id;
    j["status"] = "processing";
    resp.json(j.dump());
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
        SearchKnowledgeRequest search_request;
        search_request.query = body.value("query", "");
        search_request.top_k = body.value("top_k", 5);
        search_request.mode = parse_knowledge_search_mode(
            body.value("mode", "chunks"));

        CallerContext caller;
        caller.tenant_id = user.tenant_id;
        caller.principal_id = user.id;
        caller.principal_type = "user";
        caller.trace_id = req.header("X-Request-Id", "");

        if (!ctx_.knowledge_search_service) {
            resp.json_error(503, "RAG service not available");
            return;
        }

        auto result = ctx_.knowledge_search_service->execute(search_request, caller);

        resp.json(ToolService::knowledge_result(result).dump());
    } catch (const KnowledgeSearchError& e) {
        int status = e.code() == KnowledgeSearchErrorCode::UNAVAILABLE ? 503 : 400;
        resp.json_error(status, e.what());
    } catch (const std::exception& e) {
        resp.json_error(400, std::string("Invalid request: ") + e.what());
    }
}

}
