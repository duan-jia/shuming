// Exercise production document services; replace only persistence and unused
// inference boundaries. No live MySQL, model or file data is needed.
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "services/document_service.h"
#include "services/tool_service.h"
using namespace fastllm;
namespace { DocumentDetails fixture; }
namespace fastllm {
MysqlPool::MysqlPool(const std::string&, int, const std::string&, const std::string&, const std::string&, size_t) {}
DocumentRepository::DocumentRepository(MysqlPool& mysql) : mysql_(mysql) {}
std::vector<DocumentSummary> DocumentRepository::list_visible_documents(int tenant, bool pending) {
    if (fixture.tenant_id == tenant || (fixture.visibility == "public" && (pending || fixture.status == "ready"))) return {fixture};
    return {};
}
bool DocumentRepository::find_visible_document(int id, int tenant, DocumentDetails& document, bool pending) {
    if (id != fixture.id || list_visible_documents(tenant, pending).empty()) return false;
    document = fixture; return true;
}
bool DocumentRepository::find_document_scope(int id, int& tenant, std::string& visibility) {
    if (id != fixture.id) return false;
    tenant = fixture.tenant_id; visibility = fixture.visibility; return true;
}
bool DocumentRepository::can_cleanup_source(const std::string&) { return true; }
bool DocumentRepository::find_duplicate_document(int, const std::string&, DocumentSummary&) { return false; }
bool RagService::is_ready() const { return false; }
int RagService::enqueue_document(int, const std::string&, const std::string&, const std::string&, const std::string&, int,
    const std::string&, const std::string&, std::uint64_t, const std::string&, bool, const std::string&) { throw std::logic_error("unexpected inference call"); }
bool RagService::approve_document(int, int, const std::string&, const std::string&, const std::string&, int) { throw std::logic_error("unexpected inference call"); }
bool RagService::remove_document(int) { throw std::logic_error("unexpected inference call"); }
nlohmann::json ToolService::knowledge_result(const SearchKnowledgeResult&) { return nlohmann::json::object(); }
}
int main() {
    auto* ctx = new AppContext;
    auto* mysql = new MysqlPool("", 0, "", "", "", 1);
    ctx->document_repository = std::make_unique<DocumentRepository>(*mysql);
    DocumentService service(*ctx);
    fixture.id = 10; fixture.tenant_id = 7; fixture.uploaded_by = 2;
    fixture.filename = "document.md"; fixture.status = "ready";
    RouteParams params; params.params["id"] = "10";
    for (auto role : {Role::ADMIN, Role::USER, Role::GUEST}) {
        UserInfo user{1, 7, "member", role};
        HttpRequest upload; upload.body = R"({"filename":"new.md","content":"content","visibility":"public"})";
        HttpResponse publication; service.upload(upload, user, publication);
        assert(publication.status_code == (role == Role::ADMIN ? 503 : 403));
        HttpRequest check; check.body = std::string("{\"source_sha256\":\"") + std::string(64, 'a') + "\",\"visibility\":\"public\"}";
        HttpResponse duplicate; service.deduplicate(check, user, duplicate);
        assert(duplicate.status_code == (role == Role::ADMIN ? 200 : 403));
        upload.body = R"({"filename":"new.md","content":"content","visibility":"group"})";
        HttpResponse group_upload; service.upload(upload, user, group_upload); assert(group_upload.status_code == 503);
        // A 503 here means authorization passed but the model is deliberately offline.
        fixture.visibility = "group"; fixture.tenant_id = 7;
        HttpResponse detail; service.detail(params, user, detail);
        assert(detail.status_code == 200 && nlohmann::json::parse(detail.body)["can_delete"] == true);
        HttpResponse same_group; service.remove(params, user, same_group); assert(same_group.status_code == 503);
        fixture.tenant_id = 8;
        HttpResponse other_group; service.remove(params, user, other_group); assert(other_group.status_code == 403);
        fixture.visibility = "public";
        HttpResponse public_detail; service.detail(params, user, public_detail);
        assert(public_detail.status_code == 200);
        assert(nlohmann::json::parse(public_detail.body)["can_delete"] == (role == Role::ADMIN));
        HttpResponse public_delete; service.remove(params, user, public_delete);
        assert(public_delete.status_code == (role == Role::ADMIN ? 503 : 403));
    }
    std::cout << "document service publication, group sharing and public deletion tests passed\n";
}
