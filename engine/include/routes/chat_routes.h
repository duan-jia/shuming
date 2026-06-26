#pragma once

#include "server/http_server.h"
#include "services/chat_service.h"

namespace fastllm {

void register_chat_routes(HttpServer& server, ChatService& chat);

}
