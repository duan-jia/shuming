#include "routes/auth_routes.h"

namespace fastllm {

void register_auth_routes(HttpServer& server, AuthService& auth) {
    server.router().post("/api/auth/login", [&auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        auth.login(req, resp);
    });
}

}
