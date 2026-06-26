#pragma once

#include "app/app_context.h"
#include "security/rbac_manager.h"
#include "server/http_protocol.h"
#include "server/router.h"

namespace fastllm {

class DocumentService {
public:
    explicit DocumentService(AppContext& ctx);

    void upload(const HttpRequest& req, const UserInfo& user, HttpResponse& resp);
    void list(const UserInfo& user, HttpResponse& resp);
    void remove(const RouteParams& params, const UserInfo& user, HttpResponse& resp);
    void search(const HttpRequest& req, const UserInfo& user, HttpResponse& resp);

private:
    AppContext& ctx_;
};

}
