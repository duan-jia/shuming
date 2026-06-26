#pragma once

#include <mysql/mysql.h>

#include "app/app_context.h"
#include "security/rbac_manager.h"
#include "server/http_protocol.h"

namespace fastllm {

class AuthService {
public:
    explicit AuthService(AppContext& ctx);

    UserInfo extract_user(const HttpRequest& req) const;
    bool require_auth(const HttpRequest& req, HttpResponse& resp, UserInfo& user) const;
    bool require_permission(const UserInfo& user, const std::string& resource,
                            const std::string& action, HttpResponse& resp) const;
    void login(const HttpRequest& req, HttpResponse& resp);

private:
    AppContext& ctx_;

    static std::string escape_sql(MYSQL* mysql, const std::string& input);
};

}
