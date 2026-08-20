#include "routes/document_routes.h"

namespace fastllm {

void register_document_routes(HttpServer& server, DocumentService& documents, AuthRequired require_auth) {
    server.router().post("/api/documents", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.upload(req, user, resp);
    });

    server.router().post("/api/documents/deduplicate", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.deduplicate(req, user, resp);
    });

    server.router().get("/api/documents", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.list(user, resp);
    });

    server.router().get("/api/documents/:id", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.detail(params, user, resp);
    });

    server.router().post("/api/documents/:id/approve", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.approve(params, user, resp);
    });

    server.router().del("/api/documents/:id", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.remove(params, user, resp);
    });

    server.router().post("/api/documents/search", [&documents, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        documents.search(req, user, resp);
    });
}

}
