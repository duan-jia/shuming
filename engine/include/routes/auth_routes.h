#pragma once

#include "server/http_server.h"
#include "services/auth_service.h"

namespace fastllm {

void register_auth_routes(HttpServer& server, AuthService& auth);

}
