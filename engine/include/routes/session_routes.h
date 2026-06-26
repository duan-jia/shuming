#pragma once

#include "routes/auth_required.h"
#include "security/rbac_manager.h"
#include "server/http_server.h"
#include "services/session_service.h"

namespace fastllm {

void register_session_routes(HttpServer& server, SessionService& sessions, AuthRequired require_auth);

}
