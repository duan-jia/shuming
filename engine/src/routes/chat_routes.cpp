#include "routes/chat_routes.h"

namespace fastllm {

void register_chat_routes(HttpServer& server, ChatService& chat) {
    server.router().sse("/api/chat/stream", [&chat](const HttpRequest& req, RouteParams params, std::shared_ptr<SseContext> sse_ctx) {
        chat.stream(req, params, std::move(sse_ctx));
    });
}

}
