#pragma once

#include "routes/auth_required.h"
#include "server/http_server.h"
#include "services/admin_service.h"

namespace fastllm {

void register_admin_routes(HttpServer& server, AdminService& admin, AuthRequired require_auth);

}
