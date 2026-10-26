#pragma once

#include "app/app_context.h"
#include "security/login_limiter.h"
#include "security/rbac_manager.h"
#include "server/http_protocol.h"

namespace fastllm {

class AuthService {
public:
    explicit AuthService(AppContext& ctx);

    UserInfo extract_user(const HttpRequest& req) const;
    bool require_auth(const HttpRequest& req, HttpResponse& resp, UserInfo& user) const;
    void guest_login(const HttpRequest& req, HttpResponse& resp);
    void guest_config(HttpResponse& resp) const;
    void login(const HttpRequest& req, HttpResponse& resp);

private:
    AppContext& ctx_;
    LoginLimiter login_limiter_;
    std::string dummy_password_hash_;
};

}
