#pragma once

#include <memory>
#include <string>

#include "app/app_context.h"
#include "server/http_protocol.h"
#include "server/router.h"
#include "server/sse_connection.h"

namespace fastllm {

class ChatService {
public:
    explicit ChatService(AppContext& ctx);

    void stream(const HttpRequest& req, const RouteParams& params, std::shared_ptr<SseContext> sse_ctx);

private:
    AppContext& ctx_;

    static std::string generate_uuid();
    static void send_sse_error(const std::shared_ptr<SseContext>& sse_ctx, const std::string& message);
};

}
