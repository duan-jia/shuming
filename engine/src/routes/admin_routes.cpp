#include "routes/admin_routes.h"

namespace fastllm {

void register_admin_routes(HttpServer& server, AdminService& admin, AuthRequired require_auth) {
    server.router().post("/api/tenants", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        admin.create_tenant(req, user, resp);
    });

    server.router().get("/api/tenants", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        admin.list_tenants(user, resp);
    });

    server.router().post("/api/users", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        admin.create_user(req, user, resp);
    });

    server.router().get("/api/users", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        admin.list_users(user, resp);
    });

    server.router().post("/api/users/:id/password", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req,resp,user)) return;
        admin.reset_user_password(req,params,user,resp);
    });
    server.router().post("/api/users/:id/quota", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        admin.set_user_quota(req, params, user, resp);
    });

    server.router().del("/api/users/:id", [&admin, require_auth](const HttpRequest& req, RouteParams params, HttpResponse& resp) {
        UserInfo user;
        if (!require_auth(req, resp, user)) return;
        admin.delete_user(params, user, resp);
    });
}

}
