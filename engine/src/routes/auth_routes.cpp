#include "routes/auth_routes.h"

namespace fastllm {

void register_auth_routes(HttpServer& server, AuthService& auth) {
    server.router().get("/api/auth/config", [&auth](const HttpRequest&, RouteParams, HttpResponse& resp) {
        auth.guest_config(resp);
    });
    server.router().worker_endpoint("/api/auth/guest", [&auth](const HttpRequest& req, RouteParams, HttpResponse& resp) {
        if (req.method != HttpMethod::POST) { resp.json_error(405, "Method not allowed"); return; }
        auth.guest_login(req, resp);
    });
    server.router().worker_endpoint("/api/auth/login", [&auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        if (req.method != HttpMethod::POST) { resp.json_error(405, "Method not allowed"); return; }
        auth.login(req, resp);
    });
}

}
