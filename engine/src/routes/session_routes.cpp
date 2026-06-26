#include "routes/session_routes.h"

namespace fastllm {

void register_session_routes(HttpServer& server, SessionService& sessions, AuthRequired require_auth) {
    server.router().get("/api/sessions", [&sessions, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        sessions.list(user, resp);
    });

    server.router().post("/api/sessions", [&sessions, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        sessions.create(req, user, resp);
    });

    server.router().get("/api/sessions/:id", [&sessions, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        sessions.detail(params, user, resp);
    });

    server.router().del("/api/sessions/:id", [&sessions, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        sessions.remove(params, user, resp);
    });

    server.router().get("/api/sessions/:id/messages", [&sessions, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        sessions.messages(params, user, resp);
    });
}

}
