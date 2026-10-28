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
    void refresh(const HttpRequest& req, HttpResponse& resp);
    void logout(const HttpRequest& req, HttpResponse& resp);
    void change_password(const HttpRequest& req, HttpResponse& resp);
    void me(const HttpRequest& req, HttpResponse& resp);
    void login(const HttpRequest& req, HttpResponse& resp);

private:
    AppContext& ctx_;
    LoginLimiter login_limiter_;
    LoginLimiter refresh_limiter_;
    LoginLimiter password_limiter_;
    void issue_login(const LoginUser& user, HttpResponse& resp);
    std::string dummy_password_hash_;
};

}
