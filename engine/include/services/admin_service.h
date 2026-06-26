#pragma once

#include <mysql/mysql.h>

#include "app/app_context.h"
#include "security/rbac_manager.h"
#include "server/http_protocol.h"
#include "server/router.h"

namespace fastllm {

class AdminService {
public:
    explicit AdminService(AppContext& ctx);

    void create_tenant(const HttpRequest& req, const UserInfo& user, HttpResponse& resp);
    void list_tenants(const UserInfo& user, HttpResponse& resp);
    void create_user(const HttpRequest& req, const UserInfo& user, HttpResponse& resp);
    void list_users(const UserInfo& user, HttpResponse& resp);
    void delete_user(const RouteParams& params, const UserInfo& user, HttpResponse& resp);

private:
    AppContext& ctx_;

    static bool require_admin(const UserInfo& user, HttpResponse& resp);
    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
