#pragma once

#include <string>

#include "persistence/session_repository.h"
#include "security/rbac_manager.h"
#include "server/http_protocol.h"
#include "server/router.h"

namespace fastllm {

class SessionService {
public:
    explicit SessionService(SessionRepository& sessions);

    void list(const UserInfo& user, HttpResponse& resp);
    void create(const HttpRequest& req, const UserInfo& user, HttpResponse& resp);
    void detail(const RouteParams& params, const UserInfo& user, HttpResponse& resp);
    void remove(const RouteParams& params, const UserInfo& user, HttpResponse& resp);
    void messages(const RouteParams& params, const UserInfo& user, HttpResponse& resp);

private:
    SessionRepository& sessions_;

    static std::string generate_uuid();
};

}
